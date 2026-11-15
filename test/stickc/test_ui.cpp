#ifdef _WIN32
#include <stdlib.h>
inline int setenv(const char* k,const char* v,int){return _putenv_s(k,v);}
#endif
#include "../../ports/stickc-plus2/main.cpp"
#include <assert.h>
static uint8_t volumeMock=1;
void audioSetVolume(uint8_t v){volumeMock=v;}
uint8_t audioVolume(){return volumeMock;}
void audioSetSleeping(bool){}
void audioSetIdle(bool){}
bool audioBusy(){return false;}
void shot(const char* name){draw(millis());canvas.exportPPM(name);}
void clickA(){M5.BtnA.click=true;loop();}
// landscape, not rotated: PWR (bottom edge) = next, B (top edge) = previous
void clickNext(){M5.BtnPWR.click=true;loop();}
void clickPrev(){M5.BtnB.click=true;loop();}
void holdBack(){M5.BtnA.hold=true;loop();}
int main(){
  mockNvsReset(); mockSetMillis(100); mockSerialSilence(true); setup();
  assert(page==STARTER); shot("test/build/starter.ppm");
  clickNext();clickA();assert(page==HOME&&pet.isEgg());shot("test/build/egg.ppm");
  clickA();clickA();clickA();assert(pet.speciesId==4);
  shot("test/build/home.ppm");
  clickNext();clickNext();assert(homeSelected==2&&timeLeft(labelUntil));shot("test/build/home-label.ppm");homeSelected=0;labelUntil=0;
  { // every biome at four hours of the day, for the scene previews
    int keep=pet.speciesId; uint32_t base=1794787200UL;
    const char* when[]={"day","dawn","dusk","night"}; const int hour[]={13,7,19,22};
    for(int b=0;b<6;++b){
      int sp=1; while(DEX_TBL[sp].biome!=b) ++sp;
      pet.speciesId=sp; mon.load(sp,false);
      for(int t=0;t<4;++t){
        pet.lastSeenEpoch=base+hour[t]*3600; mockSetMillis(100000+t*7000+b*3100);
        char n[48]; snprintf(n,sizeof(n),"test/build/biome%d-%s.ppm",b,when[t]); shot(n);
      }
    }
    pet.speciesId=keep; pet.lastSeenEpoch=0;
  }
  rtcValid=false;fsReady=false;
  for(int n=0;n<6;++n){homeSelected=n;draw(millis());}
  rtcValid=fsReady=true;
  homeSelected=1;clickA();assert(page==FEED);shot("test/build/feed.ppm");
  pet.fullness=20;clickA();assert(page==HOME&&pet.fullness>20);
  homeSelected=2;clickA();assert(page==PLAY);
  gameStart=millis()-300;draw(millis());shot("test/build/play.ppm");
  mockAdvanceMillis(10001);loop();assert(page==RESULT);shot("test/build/result.ppm");clickA();assert(page==HOME);
  open(MORE); selected=0;clickA();assert(page==TRAIN);
  for(int i=0;i<24;++i){mockAdvanceMillis(160);clickA();}
  assert(hits==24);shot("test/build/training.ppm");holdBack();assert(page==RESULT&&pet.trAtk==6);
  open(HOME);pet.sleeping=true;homeSelected=2;clickA();assert(page==HOME);toastUntil=0;homeSelected=3;shot("test/build/sleep.ppm");pet.sleeping=false;
  ask(2,MORE);assert(selected==0);clickA();assert(pet.speciesId==4);
  ask(2,MORE);holdBack();assert(pet.speciesId==4&&page==MORE);
  ask(2,MORE);shot("test/build/confirm.ppm");
  pet.joy=32;pet.bond=7;saveNow();Pet restored;restored.begin();assert(restored.joy==32&&restored.bond==7);
  open(HOME);M5.BtnB.hold=true;loop();assert(screenOff);
  homeSelected=3;pet.hygiene=20;clickA();assert(!screenOff&&pet.hygiene==20);
  toastUntil=0;
  open(HOME);homeSelected=0;clickPrev();assert(homeSelected==5);clickNext();assert(homeSelected==0);
  flipped=true;M5.BtnB.click=true;loop();assert(homeSelected==1);M5.BtnPWR.click=true;loop();assert(homeSelected==0);flipped=false;
  open(MORE);clickPrev();assert(selected==7);holdBack();assert(page==HOME);
  for(Page p:{MORE,SETTINGS,FEED}){open(p);for(int i=0;i<itemCount();++i){selected=i;draw(millis());}}
  open(DEX);for(int d=1;d<=151;++d){dex=d;pet.dexReg[(d-1)/8]|=1<<((d-1)%8);draw(millis());}
  dex=4;shot("test/build/dex.ppm");dex=150;pet.dexReg[149/8]&=~(1<<(149%8));shot("test/build/dex-unknown.ppm");dex=4;open(INFO);shot("test/build/profile.ppm");
  open(MORE);selected=1;shot("test/build/more.ppm");open(SETTINGS);selected=2;shot("test/build/settings.ppm");selected=4;clickA();assert(page==CLOCK);clockField=3;shot("test/build/clock.ppm");clockField=0;
  editTime.tm_year=126;editTime.tm_mon=1;editTime.tm_mday=31;clockField=4;clickA();assert(page==CLOCK&&clockField==2);
  editTime.tm_mday=28;clockField=4;clickA();assert(page==SETTINGS);
  ask(2,MORE);clickNext();clickA();assert(pet.ceremony==CER_RELEASE);
  printf("PASS: actual port UI navigation, game deadlines, training, save/restore, wake-input suppression, clock validation, 151 dex layouts\n");
}
