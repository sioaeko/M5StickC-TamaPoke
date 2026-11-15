#include <M5Unified.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include "../../pet.h"
#include "../../dex.h"
#include "../../i18n.h"
#include "../../species.h"
#include "../../audio.h"
#include "sprites.h"

// Native 240x135 UI drawn in the original firmware's visual language: a biome
// sky that follows the real hour, a cream panel with the four need bars and
// white icon buttons, cream full-screen menus with ink text, and the upstream
// (i18n) strings. Only the layout is re-flowed for a landscape, button-only
// screen; no square framebuffer, coordinate scaling or touch emulation.
namespace {
#define C565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
constexpr uint16_t lerp565(uint16_t a, uint16_t b, int i, int n) {
  return (uint16_t)(((((a >> 11) & 31) + ((((b >> 11) & 31) - ((a >> 11) & 31)) * i / n)) << 11) |
                    ((((a >> 5) & 63) + ((((b >> 5) & 63) - ((a >> 5) & 63)) * i / n)) << 5) |
                    ((a & 31) + (((b & 31) - (a & 31)) * i / n)));
}
constexpr uint16_t BG=UI_BG_DAY, INK=UI_INK, WHITE=UI_WHITE, INK_K=0x18C4;
// UI_TRACK is the original hint colour, but on a small LCD it vanishes on cream;
// halfway to ink keeps the same hue and stays readable.
constexpr uint16_t MUTED=lerp565(UI_TRACK,UI_INK,9,16);
constexpr uint16_t GOLD=C565(0xff,0xe7,0x9f);   // selection fill (original sun / hit ring colour)
constexpr int BTN_PITCH=24, BTN_X0=120-(6*BTN_PITCH-6)/2;
constexpr int CX=120, HZ=64, PANEL_Y=92, PET_GROUND=86, GAME_HZ=106;
M5Canvas canvas(&M5.Display);
CompactMon mon, starterMon[3];
Pet pet;
Preferences prefs;
enum Page { HOME, STARTER, FEED, MORE, DEX, INFO, SETTINGS, CLOCK, CONFIRM, PLAY, TRAIN, RESULT };
Page page=HOME, returnPage=HOME, lastGame=PLAY;
int selected=0, homeSelected=0, dex=1, confirmAction=0, clockField=0;
bool dexShiny=false, screenOff=false, fsReady=false, rtcValid=false, flipped=false, charging=false;
bool gNight=false, resultNewHi=false;
uint8_t brightness=120;
uint32_t labelUntil=0, lastInput=0, lastFrame=0, gameStart=0, lastHit=0, toastUntil=0, lastClockPoll=0;
uint16_t hits=0, resultHits=0, resultGain=0;
float sackShake=0;
int batteryLevel=0;
char toast[40]={};
tm editTime={};
// Same order as the original's four round buttons (food, play, light, bath),
// with pet (a tap on the pet upstream) first and MORE (the swipes) last.
const char* homeItems[]={"PET","FEED","PLAY","LIGHT","BATH","MORE"};
const char* feedItems[]={"RED BERRY","BLUE BERRY","GREEN BERRY","CANDY"};
const char* moreItems[]={"STRENGTH TRAINING","POKEDEX","PET PROFILE","EVOLVE","FAREWELL","STAY / LET GO","RELEASE PET","SETTINGS"};
const char* settingsItems[]={"SOUND","VOLUME","BRIGHTNESS","ROTATE 180","SET CLOCK","SAVE & POWER OFF"};
const int STARTERS[]={1,4,7};

// ---------- text helpers (6x8 bitmap font, like the original's classic font) ----------
int textW(const char* s,int size) { return int(strlen(s))*6*size; }
void text(const char* s,int x,int y,uint16_t color=INK,int size=1) {
  canvas.setTextColor(color); canvas.setTextSize(size); canvas.drawString(s,x,y);
}
// centred text; drops to size 1 and then truncates so nothing leaves the screen
void ctext(const char* s,int y,uint16_t color,int size=1,int cx=CX,int maxW=236) {
  while(size>1 && textW(s,size)>maxW) --size;
  char buf[41]; snprintf(buf,sizeof(buf),"%s",s);
  int maxChars=maxW/(6*size); if(int(strlen(buf))>maxChars) buf[maxChars]=0;
  int x=cx-textW(buf,size)/2; if(x<0) x=0; if(x+textW(buf,size)>240) x=240-textW(buf,size);
  text(buf,x,y,color,size);
}
void pill(int x,int y,int w,int h,int r,uint16_t fill,uint16_t border) {
  canvas.fillRoundRect(x,y,w,h,r,fill); canvas.drawRoundRect(x,y,w,h,r,border);
}
// character-map sprite from species.h; step=2 samples every other pixel (half size)
void drawMap(const char* const* map,int n,int x,int y,int s=1,int step=1,bool sil=false) {
  for(int r=0;r<n;r+=step) {
    const char* row=map[r];
    for(int c=0;c<n;c+=step) {
      char ch=row[c]; if(ch=='.') continue;
      int run=1;
      while(c+run*step<n && row[c+run*step]==ch) ++run;
      canvas.fillRect(x+(c/step)*s,y+(r/step)*s,run*s,s,sil?INK_K:spriteColor(ch));
      c+=(run-1)*step;
    }
  }
}
void drawLevelBars(int x,int yBottom,int lit,int total,uint16_t ink) {
  for(int i=0;i<total;++i) {
    int h=3+i*2;
    if(i<lit) canvas.fillRect(x+i*5,yBottom-h,3,h,ink);
    else canvas.drawRect(x+i*5,yBottom-h,3,h,ink);
  }
}

void message(const char* s) { snprintf(toast,sizeof(toast),"%s",s); toastUntil=millis()+1800; }
void open(Page p) { page=p; selected=0; }
uint32_t epoch() {
  if(!M5.Rtc.isEnabled() || M5.Rtc.getVoltLow()) return 0;
  auto dt=M5.Rtc.getDateTime();
  if(dt.date.year<2024 || dt.date.year>2037) return 0;
  tm t={}; t.tm_year=dt.date.year-1900; t.tm_mon=dt.date.month-1; t.tm_mday=dt.date.date;
  t.tm_hour=dt.time.hours; t.tm_min=dt.time.minutes; t.tm_sec=dt.time.seconds;
  time_t value=mktime(&t);
  return value>0?uint32_t(value):0;
}
bool setEpoch(uint32_t value) {
  time_t v=value; tm t=*gmtime(&v);
  m5::rtc_datetime_t dt;
  dt.date.year=t.tm_year+1900; dt.date.month=t.tm_mon+1; dt.date.date=t.tm_mday; dt.date.weekDay=t.tm_wday;
  dt.time.hours=t.tm_hour; dt.time.minutes=t.tm_min; dt.time.seconds=t.tm_sec;
  if(!M5.Rtc.setDateTime(dt)) { message("RTC write failed"); return false; }
  rtcValid=true; pet.setClock(value); return true;
}
void saveNow() { pet.lastSeenEpoch=epoch(); pet.checkpoint(); }
bool awakePet() {
  if(pet.isEgg()) { message("Hatch your egg first"); return false; }
  if(pet.sleeping) { message("Wake your pet first"); return false; }
  return pet.ceremony==CER_NONE;
}
void beginGame(Page p) {
  if(!awakePet()) return;
  open(p); gameStart=millis(); lastHit=gameStart-250; hits=0; sackShake=0;
}
int ballPosition(uint32_t now) {
  int phase=((now-gameStart)%1200)*200/1200;
  return phase<=100?phase:200-phase;
}
void finishGame() {
  lastGame=page; resultHits=hits;
  if(page==TRAIN) {
    resultNewHi=hits>pet.strHi;
    resultGain=pet.trainStrength(hits);
  } else {
    resultNewHi=hits>pet.gameHi;
    pet.playResult((uint8_t)min(hits,uint16_t(255)));
  }
  sfxPlay(resultNewHi && hits ? SFX_MEDAL : SFX_PLAY);
  open(RESULT); saveNow();
}
void ask(int action,Page parent) { confirmAction=action; returnPage=parent; open(CONFIRM); }
void goBack() {
  if(page==STARTER) return;
  if(page==PLAY || page==TRAIN) { finishGame(); return; }
  if(page==CONFIRM) { open(returnPage); return; }
  if(page==CLOCK) { open(SETTINGS); return; }
  if(page==SETTINGS || page==DEX || page==INFO) { open(MORE); return; }
  open(HOME);
}
int itemCount() {
  switch(page) {
    case HOME:return 6; case STARTER:return 3; case FEED:return 4;
    case MORE:return 8; case SETTINGS:return 6; case CONFIRM:return 2;
    default:return 1;
  }
}
void next() {
  if(page==DEX) { dex=dex%151+1; dexShiny=false; }
  else if(page==CLOCK) {
    if(clockField==0) editTime.tm_year=editTime.tm_year>=137?124:editTime.tm_year+1;
    if(clockField==1) editTime.tm_mon=(editTime.tm_mon+1)%12;
    if(clockField==2) editTime.tm_mday=editTime.tm_mday%31+1;
    if(clockField==3) editTime.tm_hour=(editTime.tm_hour+1)%24;
    if(clockField==4) editTime.tm_min=(editTime.tm_min+1)%60;
  } else if(page==HOME) { homeSelected=(homeSelected+1)%6; labelUntil=millis()+2500; }
  else selected=(selected+1)%itemCount();
}
void prev() {
  if(page==DEX) { dex=(dex+149)%151+1; dexShiny=false; }
  else if(page==CLOCK) {
    if(clockField==0) editTime.tm_year=editTime.tm_year<=124?137:editTime.tm_year-1;
    if(clockField==1) editTime.tm_mon=(editTime.tm_mon+11)%12;
    if(clockField==2) editTime.tm_mday=(editTime.tm_mday+29)%31+1;
    if(clockField==3) editTime.tm_hour=(editTime.tm_hour+23)%24;
    if(clockField==4) editTime.tm_min=(editTime.tm_min+59)%60;
  } else if(page==HOME) { homeSelected=(homeSelected+5)%6; labelUntil=millis()+2500; }
  else selected=(selected+itemCount()-1)%itemCount();
}
void select() {
  switch(page) {
    case STARTER:
      pet.chooseStarter(STARTERS[selected]);
      for(auto& m:starterMon) m.release();
      open(HOME); break;
    case HOME:
      if(pet.ceremony!=CER_NONE) break;
      switch(homeSelected) {
        case 0:
          if(pet.isEgg()) pet.eggTap(); else if(awakePet()) { pet.caress(); message("A little love!"); }
          break;
        case 1: if(awakePet()) open(FEED); break;
        case 2: beginGame(PLAY); break;
        case 3: pet.toggleLight(); break;
        case 4: pet.clean(); message("All clean!"); break;
        case 5: open(MORE); break;
      }
      break;
    case FEED:
      if(awakePet()) { if(selected<3) pet.feedBerry(selected); else pet.feedCandy(); open(HOME); }
      break;
    case MORE:
      switch(selected) {
        case 0: beginGame(TRAIN); break;
        case 1: dex=pet.isEgg()?1:pet.speciesId; dexShiny=false; open(DEX); break;
        case 2: open(INFO); break;
        case 3: if(pet.canEvolveNow()) ask(0,MORE); else message("Not ready to evolve"); break;
        case 4: if(pet.canFarewellNow()) ask(1,MORE); else message("Not ready for farewell"); break;
        case 5: if(pet.canRunawayNow()) ask(3,MORE); else message("Your pet is staying"); break;
        case 6: if(!pet.isEgg()) ask(2,MORE); else message("Hatch your egg first"); break;
        case 7: open(SETTINGS); break;
      }
      break;
    case SETTINGS:
      switch(selected) {
        case 0: audioSetEnabled(!audioEnabled()); break;
        case 1: audioSetVolume((audioVolume()+1)%3); break;
        case 2: brightness=brightness>=200?40:brightness+40; prefs.putUChar("bright",brightness); M5.Display.setBrightness(brightness); break;
        case 3: flipped=!flipped; prefs.putBool("flip",flipped); M5.Display.setRotation(flipped?3:1); break;
        case 4: {
          time_t e=epoch(); if(!e) e=1794787200; editTime=*gmtime(&e); clockField=0; open(CLOCK); break;
        }
        case 5: ask(4,SETTINGS); break;
      }
      break;
    case CLOCK:
      if(++clockField==5) {
        tm requested=editTime; editTime.tm_sec=0;
        time_t value=mktime(&editTime);
        if(editTime.tm_mon!=requested.tm_mon || editTime.tm_mday!=requested.tm_mday) {
          editTime=requested; clockField=2; message("Invalid date; change day");
        } else if(setEpoch(value)) { open(SETTINGS); message("Clock saved"); }
        else clockField=4;
      }
      break;
    case CONFIRM:
      if(selected==1) {
        if(confirmAction==0 && pet.canEvolveNow()) pet.evolve();
        if(confirmAction==1 && pet.canFarewellNow()) pet.startFarewell();
        if(confirmAction==2 && !pet.isEgg()) pet.release();
        if(confirmAction==3 && pet.canRunawayNow()) pet.startRunaway();
        if(confirmAction==4) { saveNow(); M5.Power.powerOff(); }
        open(HOME);
      } else {
        if(confirmAction==0) pet.declineEvolve();
        if(confirmAction==1) pet.declineFarewell();
        open(returnPage);
      }
      break;
    case DEX: dexShiny=!dexShiny; break;
    case PLAY: case TRAIN: {
      uint32_t now=millis();
      if(now-gameStart>=10000 || now-lastHit<150) break;
      lastHit=now;
      if(page==TRAIN || (ballPosition(now)>=38 && ballPosition(now)<=62)) {
        ++hits; sfxPlay(SFX_PLAY);
        if(page==TRAIN) sackShake=6;
      } else sfxPlay(SFX_DENY);
      break;
    }
    case RESULT: open(HOME); break;
    default: break;
  }
}

// ---------- scene: biome of the species + real hour (original drawScene, re-flowed) ----------
int sceneHour() { uint32_t e=pet.lastSeenEpoch; return e?int((e/3600)%24):13; }
uint16_t inkColor() { return gNight?UI_INK_NIGHT:UI_INK; }
// Biome soils from the original (0 meadow 1 beach 2 forest 3 volcano 4 mountain 5 snow).
const uint16_t BIOME_SOIL[6]={
  C565(0x7e,0xc0,0x7f), C565(0xdc,0xca,0x94), C565(0x4f,0x8a,0x55),
  C565(0x8a,0x55,0x44), C565(0xa8,0x90,0x6a), C565(0xe6,0xee,0xf5),
};
const uint8_t STARS[][3]={{28,12,2},{70,26,1},{150,10,2},{176,34,1},{52,44,1},{110,20,1},
                          {226,48,1},{8,30,1},{92,6,1},{196,8,1},{134,38,1},{40,4,1}};
// scene state for the frame being drawn
bool sNight=false; int sHour=13, sHz=HZ; uint32_t sNow=0; uint16_t sSkyTop=0, sSkyBot=0, sSoil=0;

uint32_t hsh(uint32_t x) { x^=x>>16; x*=0x7feb352dU; x^=x>>15; x*=0x846ca68bU; x^=x>>16; return x; }
uint16_t shade(uint16_t c,int k) { return lerp565(c,0,k,16); }
uint16_t tint(uint16_t c,uint16_t to,int k) { return lerp565(c,to,k,16); }
// time-of-day light on any landscape colour (night blue, warm dawn/dusk)
uint16_t lit(uint16_t c) {
  if(sNight) return lerp565(c,C565(0x16,0x1c,0x30),9,16);
  if(sHour<8) return lerp565(c,C565(0xf0,0x90,0x88),3,16);
  if(sHour>=18) return lerp565(c,C565(0xf0,0x80,0x50),3,16);
  return c;
}
// distance: fade toward the sky colour at the horizon
uint16_t haze(uint16_t c,int k) { return lerp565(lit(c),sSkyBot,k,16); }
uint16_t skyAt(int y) { return lerp565(sSkyTop,sSkyBot,constrain(y,0,sHz),sHz); }
void px(int x,int y,uint16_t c) { canvas.fillRect(x,y,1,1,c); }

void skyColors(int h,bool night,uint16_t& top,uint16_t& bot) {
  if(night)      { top=C565(0x0c,0x12,0x24); bot=C565(0x1e,0x26,0x46); }
  else if(h<8)   { top=C565(0xd1,0x6a,0x86); bot=C565(0xf3,0xb8,0x7c); }
  else if(h<18)  { top=C565(0x8f,0xc8,0xea); bot=C565(0xdc,0xee,0xe6); }
  else           { top=C565(0xc7,0x5a,0x4a); bot=C565(0xf0,0xae,0x64); }
}
uint16_t biomeSoil(uint8_t biome) { return lit(BIOME_SOIL[biome<6?biome:0]); }

// rolling silhouette: column heights from two sine waves
void ridge(int base,float a1,float f1,float p1,float a2,float f2,float p2,uint16_t col,int y0=-1) {
  if(y0<0) y0=sHz;
  for(int x=0;x<240;x+=2) {
    int h=base+int(a1*sinf(x*f1+p1)+a2*sinf(x*f2+p2));
    if(h>0) canvas.fillRect(x,y0-h,2,h+1,col);
  }
}
// mountain peak with a darker shadow flank and optional snow cap
void peak(int x,int w,int h,uint16_t col,uint16_t snow,int y0) {
  canvas.fillTriangle(x,y0-h,x-w,y0,x+w,y0,col);
  canvas.fillTriangle(x,y0-h,x+w,y0,x+w/3,y0,shade(col,3));
  if(snow) {
    int sh=h/3, sw=w*sh/h;
    canvas.fillTriangle(x,y0-h,x-sw,y0-h+sh,x+sw,y0-h+sh,snow);
    canvas.fillTriangle(x-sw,y0-h+sh,x-sw/3,y0-h+sh,x-sw*2/3,y0-h+sh+2,snow);
    canvas.fillTriangle(x+sw/4,y0-h+sh,x+sw,y0-h+sh,x+sw/2,y0-h+sh+2,shade(snow,2));
  }
}
void conifer(int x,int gy,int h,uint16_t col,uint16_t snow=0) {
  canvas.fillRect(x-1,gy-h/5,2,h/5+1,lit(C565(0x5a,0x3a,0x26)));
  int w=max(3,h*2/5);
  for(int t=0;t<3;++t) {
    int ty=gy-h+t*h/4, tw=w*(t+2)/4, th=h*2/5;
    canvas.fillTriangle(x,ty,x-tw,ty+th,x+tw,ty+th,col);
    canvas.fillTriangle(x,ty,x+tw,ty+th,x+1,ty+th,shade(col,3));
    if(snow) canvas.fillTriangle(x,ty,x-tw/2,ty+th/2,x+tw/2,ty+th/2,snow);
  }
}
void roundTree(int x,int gy,uint16_t leaf) {
  uint16_t trunk=lit(C565(0x7a,0x52,0x34));
  canvas.fillRect(x-2,gy-14,4,14,trunk); canvas.fillRect(x+1,gy-14,1,14,shade(trunk,4));
  canvas.fillEllipse(x,gy,6,2,shade(biomeSoil(0),3));
  uint16_t dk=shade(leaf,4), hi=tint(leaf,WHITE,4);
  canvas.fillCircle(x-7,gy-17,7,dk); canvas.fillCircle(x+7,gy-17,7,dk); canvas.fillCircle(x,gy-23,9,dk);
  canvas.fillCircle(x-6,gy-19,6,leaf); canvas.fillCircle(x+5,gy-20,6,leaf); canvas.fillCircle(x-1,gy-25,7,leaf);
  canvas.fillCircle(x-4,gy-27,3,hi); canvas.fillCircle(x-8,gy-21,2,hi);
}
void bush(int x,int gy,int r,uint16_t leaf) {
  uint16_t dk=shade(leaf,4);
  canvas.fillCircle(x-r,gy-r+1,r,dk); canvas.fillCircle(x+r,gy-r+1,r,dk); canvas.fillCircle(x,gy-r-2,r+1,leaf);
  canvas.fillCircle(x-r+1,gy-r,r-1,leaf); canvas.fillCircle(x-1,gy-r-3,max(1,r/2),tint(leaf,WHITE,4));
}
void cloud(int cx,int cy,int s,uint16_t body,uint16_t under) {
  canvas.fillCircle(cx-s,cy+1,s*2/3+1,under); canvas.fillCircle(cx+s,cy+1,s*3/4+1,under);
  canvas.fillRoundRect(cx-s*5/3,cy,s*10/3,s*2/3+2,s/2,under);
  canvas.fillCircle(cx-s,cy,s*2/3,body); canvas.fillCircle(cx,cy-s/3,s,body); canvas.fillCircle(cx+s,cy,s*3/4,body);
  canvas.fillRoundRect(cx-s*5/3,cy-1,s*10/3,s*2/3,s/2,body);
}
// ground: hazy top band, soil, slightly darker toward the viewer, fine texture
void ground(uint16_t soil,uint8_t biome) {
  sSoil=soil;
  for(int y=sHz;y<135;y+=2) {
    uint16_t c=y<sHz+3?tint(soil,sSkyBot,5):lerp565(soil,shade(soil,3),y-sHz,135-sHz);
    canvas.fillRect(0,y,240,2,c);
  }
  uint16_t dk=shade(soil,biome==5?1:3), hi=tint(soil,WHITE,biome==5?8:3);
  for(int i=0;i<60;++i) {
    uint32_t r=hsh(i*977+biome*131);
    int x=r%240, y=sHz+4+(r>>8)%(135-sHz-4);
    px(x,y,(r>>20)&1?dk:hi);
  }
}
void tufts(uint16_t col,int n,uint32_t seed) {
  for(int i=0;i<n;++i) {
    uint32_t r=hsh(i*7919+seed);
    int x=r%236+2, y=sHz+5+(r>>9)%(135-sHz-6), h=2+(r>>17)%3;
    canvas.fillRect(x,y-h,1,h,col); canvas.fillRect(x-1,y-h+1,1,h-1,col); canvas.fillRect(x+1,y-h+1,1,h-1,col);
  }
}
bool nearPet(int x) { return x>90 && x<150; }

void drawSky() {
  for(int y=0;y<sHz;++y) canvas.fillRect(0,y,240,1,skyAt(y));
  if(sNight) {
    for(int i=0;i<12;++i) {
      auto& s=STARS[i]; if(s[1]>=sHz-6) continue;
      bool dim=((sNow/600)+i*7)%9==0;
      uint16_t c=dim?skyAt(s[1]):WHITE;
      canvas.fillRect(s[0],s[1],s[2],s[2],c);
      if(s[2]==2 && !dim) { px(s[0]-1,s[1],tint(WHITE,skyAt(s[1]),8)); px(s[0]+2,s[1]+1,tint(WHITE,skyAt(s[1]),8)); }
    }
    uint16_t moon=C565(0xe8,0xee,0xf5);
    canvas.fillCircle(212,40,12,tint(skyAt(40),moon,2));
    canvas.fillCircle(212,40,9,tint(skyAt(40),moon,4));
    canvas.fillCircle(212,40,7,moon);
    canvas.fillCircle(210,38,1,C565(0xc8,0xd0,0xdc)); canvas.fillCircle(214,43,1,C565(0xc8,0xd0,0xdc));
    canvas.fillCircle(216,36,6,skyAt(36));  // crescent
  } else if(sHour<18) {
    uint16_t sun=sHour<8?C565(0xff,0xd9,0x8a):C565(0xff,0xe7,0x9f);
    int sy=sHour<8?44:40;
    canvas.fillCircle(212,sy,14,tint(skyAt(sy),sun,3));
    canvas.fillCircle(212,sy,10,tint(skyAt(sy),sun,7));
    canvas.fillCircle(212,sy,7,sun);
    canvas.fillCircle(210,sy-2,3,tint(sun,WHITE,8));
  } else {  // setting sun sinking into the horizon (behind the hills)
    uint16_t sun=C565(0xff,0xf1,0xc8);
    canvas.fillCircle(CX,sHz-4,22,tint(skyAt(sHz-4),sun,3));
    canvas.fillCircle(CX,sHz-4,16,tint(skyAt(sHz-4),sun,7));
    canvas.fillCircle(CX,sHz-4,12,sun);
  }
  // three drifting clouds at different depths (dim at night)
  struct C { int speed,y,s,off; } cs[3]={{110,12,4,0},{70,22,6,110},{90,7,3,200}};
  for(auto& c:cs) {
    if(c.y+c.s>sHz-8) continue;
    int x=int((sNow/c.speed+c.off)%300)-30;
    uint16_t body=sNight?tint(skyAt(c.y),WHITE,3):WHITE;
    uint16_t under=sNight?tint(skyAt(c.y),WHITE,1):tint(skyAt(c.y+4),WHITE,9);
    cloud(x,c.y,c.s,body,under);
  }
}

void sceneMeadow() {
  ridge(14,4,.031f,0,3,.077f,1.3f,haze(C565(0x6a,0xa8,0x70),9));
  ridge(7,3,.05f,2,2,.11f,.4f,haze(C565(0x62,0xa8,0x66),4));
  uint16_t soil=biomeSoil(0);
  ground(soil,0);
  uint16_t leaf=lit(C565(0x4c,0x9a,0x50));
  roundTree(24,sHz+8,leaf);
  bush(214,sHz+9,5,leaf); bush(70,sHz+4,3,lit(C565(0x58,0xa8,0x5a)));
  tufts(shade(soil,5),26,11);
  static const uint16_t petals[]={C565(0xe8,0x50,0x3a),C565(0xff,0xd9,0x5e),WHITE,C565(0xf0,0x8a,0xa4)};
  for(int i=0;i<16;++i) {
    uint32_t r=hsh(i*331+7); int x=8+r%224, y=sHz+6+(r>>8)%(135-sHz-8);
    if(nearPet(x) && y<PET_GROUND+2) continue;
    canvas.fillRect(x,y,1,2,shade(soil,6));
    canvas.fillRect(x-1,y-2,3,2,lit(petals[(r>>16)%4])); px(x,y-2,lit(C565(0xff,0xd9,0x5e)));
  }
  if(!sNight) {  // a butterfly wandering over the meadow
    int bx=60+int(30*sinf(sNow*0.0011f)), by=sHz-6+int(5*sinf(sNow*0.0027f));
    bool up=(sNow/120)%2;
    uint16_t w=lit(C565(0xff,0xf1,0xc8));
    canvas.fillRect(bx-2,by-(up?2:0),2,2,w); canvas.fillRect(bx+1,by-(up?2:0),2,2,w); px(bx,by,INK_K);
  }
}
void sceneBeach() {
  int top=sHz-16;
  uint16_t far=lit(C565(0x3f,0x86,0xc0)), near=lit(C565(0x6c,0xb8,0xd8));
  for(int y=top;y<sHz;++y) canvas.fillRect(0,y,240,1,lerp565(far,near,y-top,sHz-top));
  canvas.fillRect(0,top,240,1,tint(far,sSkyBot,8));
  // distant island with a palm
  uint16_t isl=haze(C565(0x3c,0x6a,0x58),7);
  canvas.fillEllipse(56,top+1,16,3,isl);
  canvas.fillRect(58,top-7,1,6,isl);
  canvas.drawLine(58,top-7,53,top-5,isl); canvas.drawLine(58,top-7,63,top-5,isl); canvas.drawLine(58,top-7,56,top-9,isl);
  // glitter under the sun or moon
  uint16_t glit=sNight?tint(near,WHITE,6):tint(near,WHITE,11);
  for(int y=top+2;y<sHz;y+=2) {
    int w=2+(y-top)/3, x=212-w/2+int(3*sinf(sNow*0.004f+y));
    if(((sNow/200)+y)%3) canvas.fillRect(x,y,w,1,glit);
  }
  uint16_t foam=sNight?tint(near,WHITE,5):tint(near,WHITE,10);
  for(int i=0;i<4;++i) {
    int y=top+3+i*3, len=6+i*3;
    for(int x0=-60;x0<240;x0+=60) canvas.fillRect(x0+int((sNow/(70-i*10)+i*37)%60),y,len,1,foam);
  }
  uint16_t sand=biomeSoil(1);
  ground(sand,1);
  uint16_t wet=lerp565(sand,near,6,16);
  canvas.fillRect(0,sHz,240,4,wet);
  for(int x=0;x<240;x+=2) {
    int y=sHz+2+int(1.6f*sinf(x*0.07f+sNow*0.003f));
    canvas.fillRect(x,y,2,1,foam);
  }
  for(int i=0;i<10;++i) {  // shells and pebbles
    uint32_t r=hsh(i*613+3); int x=6+r%228, y=sHz+8+(r>>8)%(135-sHz-10);
    if(nearPet(x) && y<PET_GROUND+2) continue;
    uint16_t c=lit((r>>16)%2?C565(0xf0,0xb8,0xb0):C565(0xa8,0x98,0x80));
    canvas.fillRect(x,y,2,1,c); px(x,y+1,shade(c,4));
  }
  // starfish
  uint16_t sf=lit(C565(0xf5,0x86,0x3d));
  canvas.fillRect(36,sHz+19,5,1,sf); canvas.fillRect(38,sHz+17,1,5,sf); px(37,sHz+21,sf); px(39,sHz+21,sf);
  // palm tree leaning over the sand
  uint16_t bark=lit(C565(0x9a,0x6c,0x40)), leaf=lit(C565(0x3c,0x9a,0x4c));
  int bx=214, by=sHz+20;
  canvas.fillEllipse(bx-4,by,9,2,shade(sand,3));
  for(int s=0;s<10;++s) {
    int x=bx-s*s/8, y=by-s*4;
    canvas.fillRect(x-2,y-4,5,4,s%2?bark:shade(bark,3));
  }
  int tx=bx-11, ty=by-40;
  const int8_t fr[][2]={{-16,4},{-12,-6},{0,-9},{12,-5},{16,5},{-6,8},{8,8}};
  for(auto& f:fr) for(int d=0;d<2;++d) {
    canvas.drawLine(tx,ty+d,tx+f[0],ty+f[1]+d,d?shade(leaf,4):leaf);
    canvas.drawLine(tx+f[0]/2,ty+f[1]/2+d,tx+f[0],ty+f[1]+2+d,d?shade(leaf,4):leaf);
  }
  canvas.fillCircle(tx-1,ty+2,1,lit(C565(0x6a,0x46,0x26))); canvas.fillCircle(tx+2,ty+2,1,lit(C565(0x6a,0x46,0x26)));
}
void sceneForest() {
  uint16_t farC=haze(C565(0x2e,0x6a,0x44),9), midC=haze(C565(0x2a,0x62,0x3e),5);
  for(int x=-4;x<248;x+=8) conifer(x,sHz+1,10+hsh(x+5)%8,farC);
  for(int x=4;x<240;x+=13) if(!nearPet(x)) conifer(x,sHz+3,16+hsh(x+9)%10,midC);
  uint16_t soil=biomeSoil(2);
  ground(soil,2);
  uint16_t nearC=lit(C565(0x22,0x52,0x34));
  conifer(12,sHz+24,44,nearC); conifer(228,sHz+26,48,nearC); conifer(42,sHz+14,26,shade(nearC,1));
  tufts(shade(soil,5),22,29);
  // mushrooms
  for(int k=0;k<2;++k) {
    int x=k?186:62, y=sHz+(k?18:12);
    canvas.fillRect(x-1,y-3,2,3,lit(C565(0xf2,0xef,0xe1)));
    canvas.fillEllipse(x,y-4,3,2,lit(C565(0xe8,0x50,0x3a)));
    px(x-1,y-5,WHITE); px(x+2,y-4,WHITE);
  }
  if(sNight) for(int i=0;i<6;++i) {  // fireflies
    uint32_t r=hsh(i*41);
    int x=int(r%240+8*sinf(sNow*0.001f+i)), y=sHz-10+int((r>>8)%24)+int(4*sinf(sNow*0.0017f+i*2));
    if(((sNow/250)+i)%4) canvas.fillRect(x,y,2,2,C565(0xe8,0xf0,0x7a));
  }
}
void sceneVolcano() {
  ridge(9,3,.04f,1,2,.09f,0,haze(C565(0x5a,0x40,0x3c),9));
  // the volcano: cone, shadow flank, glowing crater, lava runs, smoke
  uint16_t rock=haze(C565(0x6a,0x44,0x3a),4), rockDk=shade(rock,3);
  int y0=sHz, top=sHz-27;
  canvas.fillTriangle(108,y0,62,top,62,y0,rock);
  canvas.fillRect(36,top,26,y0-top+1,rock);
  canvas.fillTriangle(36,top,-12,y0,36,y0,rock);
  canvas.fillTriangle(44,top,-12,y0,26,y0,rockDk); canvas.fillTriangle(44,top,36,top,-12,y0,rockDk);
  bool flick=(sNow/180)%2;
  uint16_t lava=C565(0xff,0x9b,0x3a), lavaHi=C565(0xff,0xd9,0x4a);
  canvas.fillEllipse(49,top,12,2,flick?lava:lavaHi);
  canvas.drawLine(55,top+1,60,top+10,lava); canvas.drawLine(60,top+10,65,top+19,shade(lava,3));
  canvas.drawLine(43,top+1,39,top+8,lava); canvas.drawLine(39,top+8,42,top+15,shade(lava,3));
  for(int k=0;k<5;++k) {
    int ph=int((sNow/45+k*30)%150);
    int r=3+ph/30, x=50+ph/6+int(2*sinf(ph*0.1f)), y=top-4-ph/3;
    if(y-r<0) continue;
    canvas.fillCircle(x,y,r,tint(lit(C565(0x8a,0x84,0x80)),skyAt(y),ph/12));
  }
  uint16_t soil=biomeSoil(3);
  ground(soil,3);
  // glowing cracks
  const int16_t cr[][4]={{14,10,26,14},{26,14,22,20},{166,16,180,13},{180,13,188,18},{60,22,70,24}};
  for(auto& c:cr) canvas.drawLine(c[0],sHz+c[1],c[2],sHz+c[3],flick?lava:shade(lava,2));
  // rocks with a lit top edge
  const int16_t rk[][3]={{36,8,7},{74,6,4},{158,10,5},{216,12,9}};
  for(auto& r:rk) {
    int x=r[0], y=sHz+r[1], s=r[2];
    canvas.fillTriangle(x-s,y,x,y-s,x+s,y,shade(soil,6));
    canvas.fillTriangle(x-s,y,x,y-s,x-s/3,y,shade(soil,4));
  }
  for(int e=0;e<8;++e) {  // embers drifting up
    int ph=int((sNow/30+e*23)%60);
    int x=20+int(hsh(e)%200)+int(3*sinf(ph*0.2f)), y=sHz+20-ph;
    if(y<0) continue;
    canvas.fillRect(x,y,ph<30?2:1,ph<30?2:1,ph<20?lavaHi:lava);
  }
}
void sceneMountain() {
  uint16_t farC=haze(C565(0x78,0x80,0x98),8), snowF=haze(WHITE,4);
  const int16_t fp[][3]={{18,26,30},{62,30,38},{108,24,26},{152,34,44},{198,26,32},{240,28,36}};
  for(auto& p:fp) peak(p[0],p[1],p[2],farC,snowF,sHz);
  uint16_t nearC=haze(C565(0x6a,0x5a,0x48),3), snowN=lit(C565(0xf2,0xf4,0xf8));
  const int16_t np[][3]={{8,22,20},{50,20,15},{196,20,17},{236,24,24}};
  for(auto& p:np) peak(p[0],p[1],p[2],nearC,p[2]>18?snowN:0,sHz+2);
  uint16_t soil=biomeSoil(4);
  ground(soil,4);
  uint16_t pine=lit(C565(0x2e,0x5c,0x40));
  conifer(30,sHz+16,22,pine); conifer(208,sHz+18,26,pine); conifer(222,sHz+12,16,shade(pine,1));
  const int16_t bl[][3]={{64,12,5},{176,8,4},{14,22,6}};
  for(auto& b:bl) {
    uint16_t g=lit(C565(0x9a,0x96,0x90));
    canvas.fillEllipse(b[0],sHz+b[1],b[2]+2,b[2],shade(g,3));
    canvas.fillEllipse(b[0]-1,sHz+b[1]-1,b[2],b[2]-1,g);
    canvas.fillRect(b[0]-b[2]/2,sHz+b[1]-b[2]+1,2,1,tint(g,WHITE,6));
  }
  tufts(lit(C565(0x6a,0x8a,0x4c)),16,47);
}
void sceneSnow() {
  ridge(13,4,.028f,2,3,.07f,0,haze(C565(0xd8,0xe4,0xf0),6));
  uint16_t pineFar=haze(C565(0x34,0x5a,0x52),8), snowFar=haze(WHITE,5);
  const int16_t fp[][3]={{4,-3,13},{17,-5,9},{31,-2,15},{52,-6,10},{66,-4,12},{80,-6,8},{160,-6,9},{174,-3,14},{190,-5,10},{208,-4,12},{236,-2,15}};
  for(auto& f:fp) conifer(f[0],sHz+f[1],f[2],pineFar,snowFar);
  ridge(6,2,.05f,0,2,.12f,1,haze(C565(0xe6,0xee,0xf5),2));
  uint16_t soil=biomeSoil(5);
  ground(soil,5);
  uint16_t blue=lit(C565(0xc4,0xd4,0xe8));
  canvas.fillEllipse(40,sHz+14,26,3,blue); canvas.fillEllipse(196,sHz+22,30,3,blue); canvas.fillEllipse(110,sHz+30,40,2,blue);
  uint16_t pine=lit(C565(0x2e,0x58,0x4c)), sn=lit(WHITE);
  conifer(16,sHz+20,32,pine,sn); conifer(226,sHz+22,36,pine,sn);
  // snowman
  int sx=196, sy=sHz+16;
  uint16_t body=lit(WHITE), edge=lit(C565(0xb8,0xc8,0xdc));
  canvas.fillCircle(sx,sy-5,6,edge); canvas.fillCircle(sx,sy-5,5,body);
  canvas.fillCircle(sx,sy-14,4,edge); canvas.fillCircle(sx,sy-14,3,body);
  px(sx-1,sy-15,INK_K); px(sx+1,sy-15,INK_K); canvas.fillRect(sx+1,sy-13,3,1,lit(C565(0xf5,0x86,0x3d)));
  canvas.fillRect(sx-4,sy-19,8,1,INK_K); canvas.fillRect(sx-2,sy-23,4,4,INK_K);
  for(int i=0;i<10;++i) {  // sparkles
    uint32_t r=hsh(i*97+5); int x=r%240, y=sHz+4+(r>>8)%(135-sHz-6);
    if(((sNow/300)+i)%5==0) { px(x,y,WHITE); px(x-1,y,blue); px(x+1,y,blue); }
  }
  for(int f=0;f<16;++f) {  // falling snow
    int x=int((f*53+sNow/40+int(4*sinf(sNow*0.002f+f)))%240), y=int((f*90+sNow/(16+f%3*4))%135);
    canvas.fillRect(x,y,f%3?1:2,f%3?1:2,sNight?tint(WHITE,sSkyTop,4):WHITE);
  }
}
// Full landscape for the pet's biome at the real hour. hz is the horizon line:
// HZ on the home screen, GAME_HZ (lower, more sky) behind the mini-games.
void drawScene(uint8_t biome,uint32_t now,bool night,int hz=HZ) {
  sNight=night; sHour=sceneHour(); sHz=hz; sNow=now;
  skyColors(sHour,night,sSkyTop,sSkyBot);
  drawSky();
  switch(biome<6?biome:0) {
    case 0: sceneMeadow(); break;
    case 1: sceneBeach(); break;
    case 2: sceneForest(); break;
    case 3: sceneVolcano(); break;
    case 4: sceneMountain(); break;
    case 5: sceneSnow(); break;
  }
}
void petShadow(int x,int gy,int rx=14) { canvas.fillEllipse(x,gy-1,rx,3,shade(sSoil,4)); }
void drawGameScene(bool night) {
  drawScene(pet.isEgg()?0:DEX_TBL[pet.speciesId].biome,millis(),night,GAME_HZ);
}

// ---------- header: battery, name, status (original drawHeader) ----------
void drawBattery() {
  int x=220, y=3, w=16, h=8, pc=batteryLevel;
  uint16_t col=charging?UI_BAR_OK:pc>=40?inkColor():pc>=15?UI_BAR_WARN:UI_BAR_BAD;
  canvas.drawRoundRect(x,y,w,h,2,col);
  canvas.fillRect(x+w,y+2,2,4,col);
  if(charging) {
    uint16_t bolt=C565(0xff,0xd9,0x4a); int bx=x+w/2;
    canvas.fillTriangle(bx+2,y+1,bx-3,y+4,bx+1,y+4,bolt);
    canvas.fillTriangle(bx-1,y+3,bx+3,y+3,bx-2,y+7,bolt);
  } else {
    int fw=(w-4)*constrain(pc,0,100)/100;
    if(fw>0) canvas.fillRect(x+2,y+2,fw,h-4,col);
  }
}
void otext(const char* s,int y,uint16_t color,uint16_t edge,int size,int cx=CX,int maxW=236) {
  const int8_t o[][2]={{-1,0},{1,0},{0,-1},{0,1},{1,1}};
  for(auto& d:o) ctext(s,y+d[1],edge,size,cx+d[0],maxW);
  ctext(s,y,color,size,cx,maxW);
}
void drawHeader(const char* name,uint16_t nameColor,const char* msg) {
  drawBattery();
  otext(name,6,nameColor,gNight?UI_BG_NIGHT:WHITE,2,CX,196);
  if(msg && *msg) ctext(msg,25,inkColor(),1);
}
const char* eggMsg() {
  switch(pet.eggCracks()) { case 0: return T(S_EGG_TOUCH); case 1: return T(S_EGG_MOVES); default: return T(S_EGG_ALMOST); }
}
const char* statusMsg() {
  if(pet.evolving()) return T(S_EVOLVING);
  if(pet.sleeping) return "Zzz...";
  if(pet.eating()) return T(S_EATING);
  if(pet.showHeart()) return T(S_LIKES);
  if(pet.fullness<25) return T(S_HUNGRY);
  if(pet.hygiene<25) return T(S_NEEDS_BATH);
  if(pet.energy<25) return T(S_EXHAUSTED);
  if(pet.joy<25) return T(S_SAD);
  if(!fsReady) return "Upload the sprite pack";
  if(!rtcValid) return "Set the clock: MORE > SETTINGS";
  if(pet.weight>60) return T(S_CHUBBY);
  if(pet.shiny && pet.ageMinutes<15) return T(S_IS_SHINY);
  return T(S_HAPPY);
}
// the original's pulsing call-to-action buttons; here they point at MORE
bool drawCallToAction(uint32_t now) {
  char buf[48]; uint16_t fill,border,ink;
  const char* nm=pet.nick[0]?pet.nick:dexName(pet.speciesId);
  if(pet.wantEvolveButton()) { snprintf(buf,sizeof(buf),"%s  > MORE",T(S_EVO_TAP)); fill=UI_BAR_BAD; border=WHITE; ink=WHITE; }
  else if(pet.canRunawayNow()) { snprintf(buf,sizeof(buf),T(S_RUNAWAY_BTN),nm); fill=C565(0x3a,0x44,0x5a); border=C565(0x70,0x80,0x98); ink=C565(0xc8,0xd2,0xe0); }
  else if(pet.wantFarewellButton()) { snprintf(buf,sizeof(buf),T(S_FAREWELL_BTN),nm); fill=UI_BAR_WARN; border=INK; ink=INK; }
  else return false;
  buf[36]=0;
  int p=(now/400)%2, w=textW(buf,1)+12+2*p;
  pill(CX-w/2,22-p,w,12+2*p,6,fill,border);
  ctext(buf,24,ink,1);
  return true;
}

// ---------- home ----------
void drawBar(int x,int y,const char* label,uint8_t val) {
  text(label,x,y,inkColor());
  int bx=x+28, bw=84, bh=7;
  uint16_t fill=val>=50?UI_BAR_OK:val>=25?UI_BAR_WARN:UI_BAR_BAD;
  canvas.fillRoundRect(bx,y,bw,bh,3,UI_TRACK);
  int fw=(bw-2)*val/100;
  if(fw>0) canvas.fillRoundRect(bx+1,y+1,fw,bh-2,2,fill);
}
void drawMoreIcon(int x,int y,uint16_t ink) { for(int i=0;i<3;++i) canvas.fillCircle(x+3+i*5,y+8,1,ink); }
void drawButtons() {
  uint16_t ink=inkColor();
  for(int i=0;i<6;++i) {
    int x=BTN_X0+i*BTN_PITCH, y=116;
    bool sel=i==homeSelected, off=pet.sleeping && i<=2;
    if(pet.sleeping && i==4) off=true;
    if(sel) y-=1;
    if(!off) canvas.fillRoundRect(x,y,18,18,5,sel?GOLD:WHITE);
    canvas.drawRoundRect(x,y,18,18,5,ink);
    if(sel) canvas.drawRoundRect(x-1,y-1,20,20,6,ink);
    if(off) continue;
    switch(i) {
      case 0: drawMap(SPR_HEART,32,x+1,y+1,1,2); break;
      case 1: drawMap(SPR_ICON_FOOD,16,x+1,y+1); break;
      case 2: drawMap(SPR_ICON_PLAY,16,x+1,y+1); break;
      case 3: drawMap(SPR_ICON_LIGHT,16,x+1,y+1); break;
      case 4: drawMap(SPR_ICON_CLEAN,16,x+1,y+1); break;
      case 5: drawMoreIcon(x+1,y+1,INK); break;
    }
  }
}
// name of the highlighted action, shown for a moment after B moves the cursor
void drawButtonLabel() {
  if(!timeLeft(labelUntil)) return;
  const char* label=homeItems[homeSelected];
  if(homeSelected==0 && pet.isEgg()) label="HATCH";
  if(homeSelected==3) label=pet.sleeping?"WAKE UP":"SLEEP";
  int bx=BTN_X0+homeSelected*BTN_PITCH+9, w=textW(label,1)+10;
  int x=constrain(bx-w/2,2,238-w);
  pill(x,100,w,12,5,INK,INK);
  canvas.fillTriangle(bx-3,111,bx+3,111,bx,114,INK);
  ctext(label,102,WHITE,1,x+w/2);
}
void drawPetScene(uint32_t now) {
  int h=sceneHour();
  gNight=pet.sleeping || h<6 || h>=20;
  drawScene(pet.isEgg()?0:DEX_TBL[pet.speciesId].biome,now,gNight);
  if(pet.ceremony) {
    const char* msg=pet.ceremony==CER_FAREWELL?T(S_FAREWELL):pet.ceremony==CER_RUNAWAY?T(S_RUNAWAY):T(S_GOODBYE);
    drawHeader(dexName(pet.speciesId),DEX_TBL[pet.speciesId].accent,msg);
    mon.load(pet.speciesId,pet.shiny); mon.draw(canvas,CX,PET_GROUND+20,0,now);
    if(pet.ceremony==CER_FAREWELL && (now/400)%2) drawMap(SPR_HEART,32,CX+18,52,1,2);
    return;
  }
  if(pet.isEgg()) {
    drawHeader(T(S_EGG_HDR),inkColor(),eggMsg());
    if(pet.eggRarity()>=R_RARO)
      ctext(pet.eggRarity()==R_LEGENDARIO?T(S_EGG_LEGEND):T(S_EGG_RARE),42,pet.eggRarity()==R_LEGENDARIO?UI_BAR_WARN:0x4C98,1);
    static const uint8_t CRACK1[][2]={{15,8},{16,9},{15,10}};
    static const uint8_t CRACK2[][2]={{11,13},{12,14},{11,15},{20,12},{19,13},{20,14}};
    int wob=pet.eggCracks()>=2 && (now/250)%4==0 ? 1 : 0;
    int x=CX-16+wob, y=PET_GROUND-32;
    petShadow(CX,PET_GROUND,12);
    drawMap(SPR_EGG,SPRITE_H,x,y);
    if(pet.eggCracks()>=1) for(auto& c:CRACK1) canvas.fillRect(x+c[0],y+c[1],1,1,INK_K);
    if(pet.eggCracks()>=2) for(auto& c:CRACK2) canvas.fillRect(x+c[0],y+c[1],1,1,INK_K);
    return;
  }
  char name[32];
  snprintf(name,sizeof(name),T(S_NAME_FMT),pet.shiny?"*":"",pet.nick[0]?pet.nick:dexName(pet.speciesId),pet.level());
  drawHeader(name,gNight?UI_INK_NIGHT:DEX_TBL[pet.speciesId].accent,nullptr);
  if(!drawCallToAction(now)) ctext(statusMsg(),25,inkColor(),1);
  petShadow(CX,PET_GROUND);
  mon.load(pet.speciesId,pet.shiny);
  unsigned act=pet.sleeping?3:pet.eating()?4:pet.evolving()?7:pet.showHeart()?8:0;
  bool flash=pet.evolving() && (now/300)%2;  // original: alternate with a silhouette
  mon.draw(canvas,CX,PET_GROUND,act,now,0,flash,INK_K);
  if(pet.showHeart()) drawMap(SPR_HEART,32,CX+16,40,1,2);
  for(unsigned i=0;i<pet.poops;++i) drawMap(SPR_POOP,32,8+i*18,PET_GROUND-14,1,2);
  if(pet.sleeping) text("Zz",CX+30,44,UI_INK_NIGHT,2);
}
void drawHome(uint32_t now) {
  drawPetScene(now);
  if(pet.ceremony) return;  // the original hides the panel during ceremonies
  canvas.fillRect(0,PANEL_Y,240,135-PANEL_Y,gNight?UI_BG_NIGHT:UI_BG_DAY);
  if(pet.isEgg()) {
    char reg[24]; snprintf(reg,sizeof(reg),T(S_POKEDEX_FMT),pet.registeredCount());
    ctext(reg,99,inkColor(),1);
  } else {
    drawBar(6,95,T(S_BAR_FOOD),pet.fullness); drawBar(124,95,T(S_BAR_JOY),pet.joy);
    drawBar(6,105,T(S_BAR_ENE),pet.energy);   drawBar(124,105,T(S_BAR_HYG),pet.hygiene);
  }
  drawButtons();
  drawButtonLabel();
}
// the original's food picker: a white rounded box of four icons over the scene
void drawFeed(uint32_t now) {
  drawHome(now);
  int x=60, y=48;
  pill(x,y,120,40,8,WHITE,INK);
  const char* const* icons[]={SPR_ICON_FOOD,SPR_ICON_BERRY_B,SPR_ICON_BERRY_G,SPR_ICON_CANDY};
  for(int i=0;i<4;++i) {
    int cx=x+6+i*28;
    if(i==selected) pill(cx-1,y+3,26,24,5,GOLD,INK);
    drawMap(icons[i],16,cx+4,y+7);
  }
  ctext(feedItems[selected],y+29,INK,1);
}

// ---------- cream full-screen pages (original menu backdrop) ----------
void menuFrame(const char* title) { canvas.fillScreen(BG); ctext(title,5,INK,2); }
void hint(const char* s) { ctext(s,125,MUTED,1); }
void pageDots(int pages,int current,int y) {
  for(int i=0;i<pages;++i) {
    int x=226-(pages-1)*8+i*8;
    if(i==current) canvas.fillCircle(x,y,2,INK); else canvas.drawCircle(x,y,2,INK);
  }
}
void settingValue(int i,int y) {
  char b[12];
  switch(i) {
    case 0: case 3: {
      bool on=i==0?audioEnabled():flipped;
      text(on?"ON":"OFF",226-textW(on?"ON":"OFF",1),y,on?UI_BAR_OK:UI_BAR_BAD); break;
    }
    case 1: drawLevelBars(208,y+8,3-audioVolume(),3,INK); break;
    case 2: drawLevelBars(198,y+8,brightness/40,5,INK); break;
    case 4: {
      uint32_t e=epoch();
      if(e) { time_t v=e; tm t=*gmtime(&v); snprintf(b,sizeof(b),"%02d:%02d",t.tm_hour,t.tm_min); }
      else snprintf(b,sizeof(b),"--:--");
      text(b,226-textW(b,1),y,MUTED); break;
    }
  }
}
void list(const char* title,const char* const* items,int count,bool settings=false) {
  menuFrame(title);
  constexpr int ROWS=6;
  int first=(selected/ROWS)*ROWS;
  for(int i=first;i<count && i<first+ROWS;++i) {
    int y=25+(i-first)*16;
    if(i==selected) pill(6,y,228,15,5,WHITE,INK);
    text(items[i],14,y+4,INK);
    if(settings) settingValue(i,y+4);
  }
  if(count>ROWS) pageDots((count+ROWS-1)/ROWS,selected/ROWS,12);
  hint("BIG:OK  SIDE:MOVE  hold BIG:BACK");
}
void drawStarter(uint32_t now) {
  menuFrame(T(S_CHOOSE_STARTER));
  for(int i=0;i<3;++i) {
    int x=8+i*76, y=26;
    uint16_t accent=DEX_TBL[STARTERS[i]].accent;
    canvas.fillRoundRect(x,y,72,86,8,lerp565(accent,WHITE,6,8));
    canvas.drawRoundRect(x,y,72,86,8,accent);
    if(i==selected) { canvas.drawRoundRect(x-1,y-1,74,88,9,INK); canvas.drawRoundRect(x-2,y-2,76,90,10,INK); }
    starterMon[i].load(STARTERS[i],false);
    starterMon[i].draw(canvas,x+36,y+66,0,i==selected?now:0,0,false,INK_K,2,60);
    ctext(dexName(STARTERS[i]),y+72,INK,1,x+36,70);
  }
  hint("BIG:CHOOSE  SIDE:MOVE");
}
void drawDex(uint32_t now) {
  canvas.fillScreen(BG);
  char s[40];
  snprintf(s,sizeof(s),T(S_POKEDEX_FMT),pet.registeredCount()); text(s,6,5,MUTED);
  const char* form=dexShiny?"SHINY":"NORMAL";
  text(form,234-textW(form,1),5,dexShiny?UI_BAR_WARN:MUTED);
  bool known=dexShiny?pet.isShinyRegistered(dex):pet.isRegistered(dex);
  snprintf(s,sizeof(s),"N.%03d %s%s",dex,pet.isShinyRegistered(dex)?"*":"",pet.isRegistered(dex)?dexName(dex):"???");
  ctext(s,18,pet.isRegistered(dex)?DEX_TBL[dex].accent:INK,2);
  mon.load(dex,dexShiny);
  mon.draw(canvas,CX,118,0,known?now:0,0,!known,INK_K,3,80);
  hint("BIG:SHINY  SIDE:MOVE  hold BIG:BACK");
}
void drawCardStat(int y,const char* label,unsigned val,unsigned maxBar,uint16_t color) {
  text(label,100,y,INK);
  char n[8]; snprintf(n,sizeof(n),"%u",val); text(n,234-textW(n,1),y,INK);
  int bx=132, bw=78, fw=int(min(val,maxBar)*bw/maxBar);
  canvas.fillRoundRect(bx,y,bw,7,3,UI_TRACK);
  if(fw>2) canvas.fillRoundRect(bx,y,fw,7,3,color);
}
void drawInfo(uint32_t now) {
  canvas.fillScreen(BG);
  if(pet.isEgg()) {
    ctext(T(S_EGG_HDR),5,INK,2);
    ctext(eggMsg(),50,INK,1);
    hint("hold BIG:BACK");
    return;
  }
  char s[48];
  snprintf(s,sizeof(s),T(S_NAME_FMT),pet.shiny?"*":"",pet.nick[0]?pet.nick:dexName(pet.speciesId),pet.level());
  ctext(s,5,DEX_TBL[pet.speciesId].accent,2);
  if(pet.nick[0]) { snprintf(s,sizeof(s),"(%s)",dexName(pet.speciesId)); ctext(s,22,MUTED,1); }
  mon.load(pet.speciesId,pet.shiny); mon.draw(canvas,50,92,0,now,0,false,INK_K,2,60);
  drawCardStat(34,T(S_STAT_ATK),pet.atkStat(),260,UI_BAR_BAD);
  drawCardStat(46,T(S_STAT_DEF),pet.defStat(),260,0x4C98);
  drawCardStat(58,T(S_STAT_SPE),pet.speStat(),260,UI_BAR_WARN);
  drawCardStat(70,T(S_STAT_WGT),pet.weight,100,0xB3C8);
  drawCardStat(82,T(S_VIN),pet.bond,100,C565(0xd4,0x52,0x7e));
  // streak with a flame, as on the original profile card
  int fx=8, fy=97;
  canvas.fillTriangle(fx+4,fy,fx,fy+9,fx+8,fy+9,UI_BAR_BAD);
  canvas.fillTriangle(fx+4,fy+4,fx+2,fy+9,fx+6,fy+9,UI_BAR_WARN);
  snprintf(s,sizeof(s),T(S_STREAK_FMT),pet.streak,pet.bestStreak); text(s,fx+12,fy+1,INK);
  const char* berry=!pet.berryKnown?T(S_BERRY_UNK):pet.lovesBerry(0)?T(S_BERRY_RED):pet.lovesBerry(1)?T(S_BERRY_BLUE):T(S_BERRY_GREEN);
  snprintf(s,sizeof(s),T(S_INFO_FMT),berry,(unsigned long)(pet.ageMinutes/1440)); text(s,8,110,INK);
  hint("USB NAME <text>: rename  hold BIG:BACK");
}
// the original's decision dialog: white box, question, action / keep buttons
void drawConfirm(uint32_t now) {
  drawPetScene(now);
  canvas.fillRect(0,PANEL_Y,240,135-PANEL_Y,gNight?UI_BG_NIGHT:UI_BG_DAY);
  char q[48]; const char *yes,*no,*note; uint16_t cy,ty,cn,tn;
  const char* nm=pet.isEgg()?"":dexName(pet.speciesId);
  switch(confirmAction) {
    case 0: snprintf(q,sizeof(q),"%s",T(S_EVO_Q)); yes=T(S_EVO_TAP); no=T(S_EVO_KEEP); note="You can keep this form.";
            cy=UI_BAR_BAD; ty=WHITE; cn=UI_TRACK; tn=INK; break;
    case 1: snprintf(q,sizeof(q),"%s",T(S_FAR_Q)); yes=T(S_FAR_GO); no=T(S_FAR_STAY); note="A new egg follows goodbye.";
            cy=UI_BAR_WARN; ty=INK; cn=UI_BAR_OK; tn=WHITE; break;
    case 2: snprintf(q,sizeof(q),T(S_RELEASE_FMT),nm); yes=T(S_YES); no=T(S_NO); note="A new egg follows goodbye.";
            cy=UI_BAR_OK; ty=WHITE; cn=UI_BAR_BAD; tn=WHITE; break;
    case 3: snprintf(q,sizeof(q),"Let %s go?",nm); yes="LET GO"; no=T(S_FAR_STAY); note="A new egg follows goodbye.";
            cy=C565(0x3a,0x44,0x5a); ty=C565(0xc8,0xd2,0xe0); cn=UI_BAR_OK; tn=WHITE; break;
    default: snprintf(q,sizeof(q),"Save & power off?"); yes="POWER OFF"; no="CANCEL"; note="Pet progress will be saved.";
            cy=INK; ty=WHITE; cn=UI_TRACK; tn=INK; break;
  }
  pill(26,2,188,128,10,WHITE,INK);
  ctext(q,18,INK,1,CX,176);
  ctext(note,31,MUTED,1,CX,176);
  const int bx=40, bw=160, bh=22;
  struct { int y; uint16_t c,t; const char* label; int idx; } btn[2]={{48,cy,ty,yes,1},{78,cn,tn,no,0}};
  for(auto& b:btn) {
    canvas.fillRoundRect(bx,b.y,bw,bh,6,b.c);
    if(selected==b.idx) { canvas.drawRoundRect(bx-2,b.y-2,bw+4,bh+4,8,INK); canvas.drawRoundRect(bx-3,b.y-3,bw+6,bh+6,9,INK); }
    ctext(b.label,b.y+7,b.t,1,CX,bw-8);
  }
  ctext("BIG:OK  SIDE:SWITCH",112,MUTED,1);
}
void drawClock() {
  menuFrame(T(S_SET_TIME));
  char s[24];
  snprintf(s,sizeof(s),"%04d-%02d-%02d",editTime.tm_year+1900,editTime.tm_mon+1,editTime.tm_mday);
  snprintf(s+12,sizeof(s)-12,"%02d:%02d",editTime.tm_hour,editTime.tm_min);
  // highlight the field being edited with the original's white pill
  struct { int x,y,w,h; } f[5]={{60,32,48,16},{120,32,24,16},{156,32,24,16},{75,58,36,24},{129,58,36,24}};
  auto& r=f[clockField];
  pill(r.x-4,r.y-3,r.w+8,r.h+6,6,WHITE,INK);
  text(s,60,32,INK,2);
  text(s+12,75,58,INK,3);
  const char* fields[]={"YEAR","MONTH","DAY",T(S_HOUR),T(S_MIN)};
  ctext(fields[clockField],92,MUTED,1);
  canvas.fillRoundRect(90,103,60,16,6,clockField==4?UI_BAR_OK:UI_TRACK);
  ctext(clockField==4?"BIG: OK":"BIG: NEXT",107,clockField==4?BG:INK,1);
  hint("SIDE:-1/+1  hold BIG:BACK");
}
void drawPlay(uint32_t now) {
  bool night=sceneHour()<6 || sceneHour()>=20;
  uint16_t ink=night?UI_INK_NIGHT:UI_INK, edge=night?UI_BG_NIGHT:WHITE;
  drawGameScene(night);
  char s[24];
  snprintf(s,sizeof(s),"%u",hits); otext(s,4,ink,edge,3);
  if(page==PLAY) {
    snprintf(s,sizeof(s),T(S_REC_FMT),pet.gameHi); otext(s,30,ink,edge,1);
    int x0=76, w=148, y=56;
    canvas.fillRoundRect(x0,y,w,10,5,UI_TRACK);
    canvas.fillRoundRect(x0+w*38/100,y,w*24/100,10,4,UI_BAR_OK);
    canvas.drawRoundRect(x0,y,w,10,5,ink);
    int bx=x0+ballPosition(now)*w/100;
    uint32_t ht=now-lastHit;
    if(ht<260) {
      int rad=10+int(ht/40);
      canvas.drawCircle(bx,y+5,rad,GOLD); canvas.drawCircle(bx,y+5,rad-1,C565(0xff,0xd9,0x8a));
    }
    drawMap(SPR_ICON_PLAY,16,bx-8,y-3);
    otext("BIG: catch in the green",78,ink,edge,1,150,170);
    petShadow(40,GAME_HZ+4,10); mon.load(pet.speciesId,pet.shiny); mon.draw(canvas,40,GAME_HZ+4,2,now,1);
  } else {
    // the original strength bag: rope, chain, red bag that swings on each hit
    sackShake*=0.84f;
    int off=int(sackShake*sinf(now*0.05f)), sx=160+off, top=18;
    canvas.fillRect(158,0,3,top-8,ink);
    canvas.fillRect(sx-2,top-10,4,12,ink);
    canvas.fillRoundRect(sx-16,top,32,58,10,C565(0xb5,0x3a,0x3a));
    canvas.fillRoundRect(sx-16,top,32,9,7,C565(0x7e,0x28,0x28));
    canvas.drawRoundRect(sx-16,top,32,58,10,ink);
    canvas.fillRect(sx-16,top+27,32,2,C565(0x7e,0x28,0x28));
    otext(T(S_HIT_FAST),36,ink,edge,1,72,130);
    snprintf(s,sizeof(s),T(S_RECORD_FMT),pet.strHi); otext(s,48,ink,edge,1,72,130);
    petShadow(104,GAME_HZ+4,10); mon.load(pet.speciesId,pet.shiny); mon.draw(canvas,104,GAME_HZ+4,6,now,1);
  }
  // time bar (original): track + green remaining
  uint32_t left=now-gameStart>=10000?0:10000-(now-gameStart);
  int bw=200, fw=int(bw*left/10000);
  canvas.fillRoundRect(CX-bw/2,114,bw,6,3,UI_TRACK);
  if(fw>2) canvas.fillRoundRect(CX-bw/2,114,fw,6,3,UI_BAR_OK);
  otext("BIG:HIT  hold BIG:FINISH",125,ink,edge,1);
}
void drawResult() {
  bool night=sceneHour()<6 || sceneHour()>=20;
  uint16_t ink=night?UI_INK_NIGHT:UI_INK, edge=night?UI_BG_NIGHT:WHITE;
  drawGameScene(night);
  char s[32];
  if(lastGame==TRAIN) {
    snprintf(s,sizeof(s),T(S_HITS_FMT),resultHits); otext(s,20,ink,edge,3);
    snprintf(s,sizeof(s),T(S_STR_GAIN_FMT),resultGain); otext(s,50,UI_BAR_BAD,edge,2);
    if(resultNewHi && resultHits) otext(T(S_NEW_RECORD),74,UI_BAR_WARN,edge,1);
    else { snprintf(s,sizeof(s),T(S_RECORD_FMT),pet.strHi); otext(s,74,ink,edge,1); }
  } else {
    snprintf(s,sizeof(s),T(S_SCORE_FMT),resultHits); otext(s,24,ink,edge,3);
    if(resultNewHi && resultHits) otext(T(S_NEW_RECORD),56,UI_BAR_WARN,edge,1);
    else { snprintf(s,sizeof(s),T(S_RECORD_FMT),pet.gameHi); otext(s,56,ink,edge,1); }
    otext(resultHits>=10?T(S_GREAT_JOY):T(S_PLUS_JOY),72,ink,edge,1);
  }
  otext("Progress saved   BIG:HOME",125,ink,edge,1);
}
void draw(uint32_t now) {
  gNight=false;
  switch(page) {
    case HOME: drawHome(now); break;
    case STARTER: drawStarter(now); break;
    case FEED: drawFeed(now); break;
    case MORE: list("MORE",moreItems,8); break;
    case SETTINGS: list("SETTINGS",settingsItems,6,true); break;
    case DEX: drawDex(now); break;
    case INFO: drawInfo(now); break;
    case CONFIRM: drawConfirm(now); break;
    case CLOCK: drawClock(); break;
    case PLAY: case TRAIN: drawPlay(now); break;
    case RESULT: drawResult(); break;
  }
  if(timeLeft(toastUntil)) {
    int w=min(textW(toast,1)+16,236);
    pill(CX-w/2,PANEL_Y-18,w,15,7,WHITE,INK);
    ctext(toast,PANEL_Y-14,INK,1);
  }
  canvas.pushSprite(0,0);
}
void serialInput() {
  static String line;
  while(Serial.available()) {
    char c=Serial.read();
    if(c=='\r') continue;
    if(c!='\n') { if(line.length()<80) line+=c; else line=""; continue; }
    if(line.startsWith("TIME ")) {
      char* end=nullptr; uint32_t e=strtoul(line.c_str()+5,&end,10);
      if(end && !*end && e>=1704067200UL && e<2145916800UL) {
        Serial.println(setEpoch(e)?"OK TIME":"ERR RTC write failed");
      } else Serial.println("ERR TIME range 2024..2037");
    } else if(line.startsWith("NAME ")) {
      bool valid=line.length()>5 && line.length()<=16;
      for(unsigned i=5;i<line.length();++i) if(line[i]<32 || line[i]>126) valid=false;
      if(valid) { pet.rename(line.c_str()+5); Serial.println("OK NAME"); }
      else Serial.println("ERR NAME 1..11 printable ASCII");
    } else if(line=="SOUND OFF" || line=="SOUND ON") {
      audioSetEnabled(line=="SOUND ON");
      Serial.println(audioEnabled()?"OK SOUND ON":"OK SOUND OFF");
    } else if(line=="HEALTH") {
      Serial.printf("StickC Plus2 240x135 flash=%u psram=%u heap=%u fs=%d rtc=%d dex=%d\n",
        ESP.getFlashChipSize(),ESP.getPsramSize(),ESP.getFreeHeap(),fsReady,rtcValid,pet.speciesId);
    } else Serial.println("Commands: HEALTH | TIME <local-wall-clock epoch> | NAME <1..11 ASCII> | SOUND ON|OFF");
    line="";
  }
}
}

void setup() {
  // The Plus2 must latch battery power before peripheral setup.
  pinMode(4,OUTPUT); digitalWrite(4,HIGH);
  auto cfg=M5.config();
  cfg.internal_imu=false; cfg.internal_mic=false; cfg.internal_spk=true;
  cfg.clear_display=true;
  M5.begin(cfg);
  Serial.begin(115200);
  setenv("TZ","UTC0",1); tzset(); // RTC holds local wall time; no Wi-Fi or timezone guessing.
  prefs.begin("stick-ui",false);
  brightness=constrain(prefs.getUChar("bright",120),40,200); flipped=prefs.getBool("flip",false);
  M5.Display.setRotation(flipped?3:1); M5.Display.setBrightness(brightness);
  canvas.setColorDepth(16);
  if(!canvas.createSprite(240,135)) {
    M5.Display.fillScreen(BG); M5.Display.println("Framebuffer allocation failed");
    while(true) delay(1000);
  }
  canvas.setTextFont(1); canvas.setTextWrap(false);
  // big front key: click = OK, hold = back; side keys: click = move, hold = screen off
  M5.BtnA.setHoldThresh(600); M5.BtnB.setHoldThresh(800); M5.BtnPWR.setHoldThresh(800);
  audioBegin(); randomSeed(esp_random());
  fsReady=LittleFS.begin(false) && LittleFS.exists("/monsters.pack");
  pet.begin(); uint32_t e=epoch(); rtcValid=e!=0; if(e) pet.syncClock(e);
  batteryLevel=M5.Power.getBatteryLevel(); charging=M5.Power.isCharging();
  page=pet.awaitingStarter()?STARTER:HOME;
  lastInput=millis();
  Serial.println("TamaPoke StickC Plus2 port 0.1.0; HEALTH for diagnostics");
}
void loop() {
  M5.update(); uint32_t now=millis();
  if(now-lastClockPoll>=1000) {
    lastClockPoll=now; pet.lastSeenEpoch=epoch(); rtcValid=pet.lastSeenEpoch!=0;
    batteryLevel=M5.Power.getBatteryLevel(); charging=M5.Power.isCharging();
  }
  pet.update(now); audioSetSleeping(pet.sleeping);
  serialInput();
  // Landscape: the B key sits on the top edge and PWR on the bottom edge
  // (swapped when the screen is rotated 180), so bottom = next, top = previous.
  bool a=M5.BtnA.wasClicked(), back=M5.BtnA.wasHold();
  bool bKey=M5.BtnB.wasClicked(), pKey=M5.BtnPWR.wasClicked();
  bool off=M5.BtnB.wasHold() || M5.BtnPWR.wasHold();
  bool down=flipped?bKey:pKey, up=flipped?pKey:bKey;
  if(a || back || down || up || off) {
    lastInput=now;
    if(screenOff) { screenOff=false; M5.Display.setBrightness(brightness); }  // wake only; no action
    else if(off) { saveNow(); screenOff=true; M5.Display.setBrightness(0); }
    else {
      M5.Display.setBrightness(brightness);
      if(back) goBack(); else if(down) next(); else if(up) prev(); else if(a) select();
    }
  }
  now=millis(); // Actions can start a session or write NVS; refresh before deadlines.
  if((page==PLAY || page==TRAIN) && now-gameStart>=10000) finishGame();
  if(pet.awaitingStarter() && page!=STARTER) open(STARTER);
  if(pet.savePending() && now-lastInput>1500) pet.flushSave();
  if(!screenOff && now-lastInput>30000) M5.Display.setBrightness(25);
  if(!screenOff && now-lastFrame>=50) { lastFrame=now; draw(now); }
  delay(screenOff?20:5);
}
