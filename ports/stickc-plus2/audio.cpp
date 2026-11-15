#include <M5Unified.h>
#include <Preferences.h>
#include "../../audio.h"
static bool enabled=true, asleep=false;
static uint8_t volume=1;
static Preferences settings;
void audioBegin() {
  settings.begin("stick-audio",false);
  enabled=settings.getBool("enabled",true);
  volume=settings.getUChar("volume",1);
  M5.Speaker.setVolume(volume==0?90:volume==1?50:25);
}
void audioSetEnabled(bool on) { enabled=on; settings.putBool("enabled",on); if(!on) M5.Speaker.stop(); }
bool audioEnabled() { return enabled; }
void audioSetVolume(uint8_t v) { volume=v%3; settings.putUChar("volume",volume); M5.Speaker.setVolume(volume==0?90:volume==1?50:25); }
uint8_t audioVolume() { return volume; }
void audioSetSleeping(bool v) { asleep=v; }
void audioSetIdle(bool) {}
bool audioBusy() { return M5.Speaker.isPlaying(); }
void sfxPlay(uint8_t id) {
  static const uint16_t tones[]={900,1200,1600,1800,2200,2500,2100,300,700,1900};
  if(enabled && !asleep && id<SFX_COUNT) M5.Speaker.tone(tones[id],id==SFX_TAP?25:110);
}
