#include "calibration.h"
static constexpr float kTEMP_Af = 1.000f;
static constexpr float kTEMP_Bf = -4.000f;
static constexpr float kHUM_Af  = 1.000f;
static constexpr float kHUM_Bf  = 0.000f;
#include <math.h> // si tu gardes des utilitaires maths ici

static const char* NS = "cal";   // namespace NVS
CalAll gCal;

static CalAB composeOne(const CalPair& c){
  CalAB k;
  k.A = c.Af * c.Ag;
  k.B = (c.Ag * c.Bf) + c.Bg + c.dUser;
  return k;
}

static void loadOne(Preferences& p, const char* key, CalPair& out, bool keepFactory){
  String pfx = String(key) + "/";
  if (!keepFactory) {
    out.Af = p.getFloat((pfx + "Af").c_str(), out.Af);
    out.Bf = p.getFloat((pfx + "Bf").c_str(), out.Bf);
  }
  out.Ag    = p.getFloat((pfx + "Ag").c_str(), out.Ag);
  out.Bg    = p.getFloat((pfx + "Bg").c_str(), out.Bg);
  out.dUser = p.getFloat((pfx + "dU").c_str(), out.dUser);
}

static void saveGuided(Preferences& p, const char* key, float Ag, float Bg){
  String pfx = String(key) + "/";
  p.putFloat((pfx + "Ag").c_str(), Ag);
  p.putFloat((pfx + "Bg").c_str(), Bg);
}

static void saveUser(Preferences& p, const char* key, float dU){
  String pfx = String(key) + "/";
  p.putFloat((pfx + "dU").c_str(), dU);
}

void calCompose(){
  gCal.kT = composeOne(gCal.temp);
  gCal.kH = composeOne(gCal.hum);
}

void calInit(){
  // 1) usine immuable en dur
  gCal.temp.Af = kTEMP_Af;
  gCal.temp.Bf = kTEMP_Bf;
  gCal.hum.Af  = kHUM_Af;
  gCal.hum.Bf  = kHUM_Bf;

  // 2) guidé + user depuis NVS (Ag, Bg, dUser seulement)
  Preferences p;
  if (p.begin(NS, true)) {
    loadOne(p, "t", gCal.temp, /*keepFactory=*/true);
    loadOne(p, "h", gCal.hum,  /*keepFactory=*/true);
    p.end();
  }
  calCompose();
}


// ——— setters (avec garde-fous simples) ———
static inline float clamp(float v, float lo, float hi){ return v<lo?lo:(v>hi?hi:v); }

void calSaveGuidedTemp(float Ag, float Bg){
  // bornes raisonnables pour T
  Ag = clamp(Ag, 0.97f, 1.03f);
  Bg = clamp(Bg, -5.0f, 5.0f);
  gCal.temp.Ag = Ag; gCal.temp.Bg = Bg;
  Preferences p; if (p.begin(NS,false)) { saveGuided(p,"t",Ag,Bg); p.end(); }
  calCompose();
}
void calSaveGuidedHum(float Ag, float Bg){
  // bornes serrées pour HR
  Ag = clamp(Ag, 0.95f, 1.05f);
  Bg = clamp(Bg, -10.0f, 10.0f);
  gCal.hum.Ag = Ag; gCal.hum.Bg = Bg;
  Preferences p; if (p.begin(NS,false)) { saveGuided(p,"h",Ag,Bg); p.end(); }
  calCompose();
}
void calSetUserTemp(float d){
  d = clamp(d, -3.0f, 3.0f);
  gCal.temp.dUser = d;
  Preferences p; if (p.begin(NS,false)) { saveUser(p,"t",d); p.end(); }
  calCompose();
}
void calSetUserHum(float d){
  d = clamp(d, -10.0f, 10.0f);
  gCal.hum.dUser = d;
  Preferences p; if (p.begin(NS,false)) { saveUser(p,"h",d); p.end(); }
  calCompose();
}

void calResetGuidedTemp(){ calSaveGuidedTemp(1.0f, 0.0f); }
void calResetGuidedHum(){  calSaveGuidedHum (1.0f, 0.0f); }
void calResetUserTemp(){   calSetUserTemp(0.0f); }
void calResetUserHum(){    calSetUserHum (0.0f); }

float calApplyTemp(float rawT){ return gCal.kT.A * rawT + gCal.kT.B; }
float calApplyHum(float rawH){ return gCal.kH.A * rawH + gCal.kH.B; }
