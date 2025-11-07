#pragma once
#include <Preferences.h>

struct CalPair {
  float Af = 1.0f;  // usine (immuable en app)
  float Bf = 0.0f;
  float Ag = 1.0f;  // guidé (bouton calibrer)
  float Bg = 0.0f;
  float dUser = 0.0f; // tweak utilisateur (offset simple)
};

struct CalAB { float A=1.0f; float B=0.0f; };

struct CalAll {
  CalPair temp;   // calibration température
  CalPair hum;    // (optionnel) calibration humidité
  CalAB kT;       // A/B composés pour T
  CalAB kH;       // A/B composés pour HR
};

void calInit();                 // charge NVS et compose
void calCompose();              // recalcule kT/kH depuis paires
void calSaveGuidedTemp(float Ag, float Bg);
void calSaveGuidedHum(float Ag, float Bg);
void calSetUserTemp(float d);
void calSetUserHum(float d);
void calResetGuidedTemp();
void calResetGuidedHum();
void calResetUserTemp();
void calResetUserHum();
float rhTempCompensate();
float calApplyTemp(float rawT); // A_total*T + B_total
float calApplyHum(float rawH);  // idem (si tu décides de corriger HR)

extern CalAll gCal;
