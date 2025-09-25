// tools/decode-qrcode.cjs
const fs = require("fs");
const fetch = global.fetch; // Node 18+ natif
const args = process.argv.slice(2);

const options = Object.fromEntries(
  args
    .map(a => a.match(/^--([^=]+)=(.*)$/))
    .filter(Boolean)
    .map(([, k, v]) => [k, v])
);

const API_URL = process.env.API_URL || "https://breezly-backend.onrender.com";
const TOKEN   = process.env.TOKEN || "";

// --- NEW: helper pour inverser par paires hex (ex: 6825DD40D384 -> 84D340DD2568)
function reverseHexPairs(h) {
  return h.match(/../g).reverse().join("");
}
// --- NEW: si name suit PROV_ + 12 hexdigits, on inverse uniquement la partie MAC pour le QR
function computeQrName(name) {
  const m = String(name || "").match(/^PROV_([0-9A-Fa-f]{12})$/);
  if (!m) return name;
  const mac = m[1].toUpperCase();
  const macRev = reverseHexPairs(mac);
  return `PROV_${macRev}`;
}

const originalName = options.name || "PROV_FALLBACK";
const qrName = computeQrName(originalName);

const sensorPayload = {
  // 🔁 On envoie AU BACK la version inversée pour que le QR encode ce nom-là
  name:      qrName,
  type:      options.type     || "temperature",
  location:  options.location || "Bureau",
};

// on nomme le fichier selon ce qui est réellement encodé dans le QR
const outFile = `qr_${sensorPayload.name}.png`;

function dataUrlToBuffer(dataUrl) {
  const base64 = dataUrl.replace(/^data:image\/png;base64,/, "");
  return Buffer.from(base64, "base64");
}
function headersJson() {
  const h = { "Content-Type": "application/json" };
  if (TOKEN) h.Authorization = `Bearer ${TOKEN}`;
  return h;
}
function headersImage() {
  const h = { Accept: "image/png" };
  if (TOKEN) h.Authorization = `Bearer ${TOKEN}`;
  return h;
}

async function fetchQrById(id) {
  const urls = [
    `${API_URL}/api/sensors/${id}/qrcode`,
    `${API_URL}/api/internal/sensors/${id}/qrcode`,
  ];
  for (const url of urls) {
    const resp = await fetch(url, { headers: headersImage() });
    if (!resp.ok) continue;
    const ct = resp.headers.get("content-type") || "";
    if (ct.includes("image/png")) {
      return Buffer.from(await resp.arrayBuffer());
    }
    if (ct.includes("application/json")) {
      const json = await resp.json();
      const dataUrl = json.qrCode || json.qrcode || json.dataUrl || json.dataURL;
      if (typeof dataUrl === "string" && dataUrl.startsWith("data:image/png;base64,")) {
        return dataUrlToBuffer(dataUrl);
      }
    }
  }
  throw new Error("Impossible de récupérer le QR code via les endpoints connus.");
}

async function main() {
  const resp = await fetch(`${API_URL}/api/internal/create-sensor`, {
    method: "POST",
    headers: headersJson(),
    body: JSON.stringify(sensorPayload),
  });
  if (!resp.ok) {
    const text = await resp.text().catch(() => "");
    throw new Error(`Create sensor failed (${resp.status}): ${text}`);
  }

  const created = await resp.json();

  // QR inline ?
  const inlined =
    created.qrCode ||
    created.qrcode ||
    created.qr ||
    (created.data && (created.data.qrCode || created.data.qrcode));

  if (typeof inlined === "string" && inlined.startsWith("data:image/png;base64,")) {
    await fs.promises.writeFile(outFile, dataUrlToBuffer(inlined));
    process.stdout.write(`QR: ${outFile}\n`);
    return;
  }

  // Sinon via /qrcode
  const id =
    created.id ||
    created._id ||
    (created.sensor && (created.sensor.id || created.sensor._id));
  if (!id) throw new Error("Aucun id de capteur trouvé dans la réponse de création.");

  const buf = await fetchQrById(id);
  await fs.promises.writeFile(outFile, buf);
  process.stdout.write(`QR: ${outFile}\n`);
}

main().catch((err) => {
  process.stderr.write(`ERR ${err?.message || err}\n`);
  process.exit(1);
});
