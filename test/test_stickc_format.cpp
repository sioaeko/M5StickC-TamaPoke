// Standalone test: exercise the same decoder the ESP32 executes against every asset.
#include "../ports/stickc-plus2/sprite_format.h"
#include <assert.h>
#include <fstream>
#include <iterator>
#include <vector>
#include <stdio.h>
int main(int argc,char** argv) {
  assert(argc==2);
  std::ifstream f(argv[1],std::ios::binary);
  std::vector<uint8_t> pack((std::istreambuf_iterator<char>(f)),{});
  assert(pack.size()>1216 && !memcmp(pack.data(),"SPM1",4));
  unsigned actions=0, frames=0;
  size_t maxDecoded=0;
  for(int i=0;i<302;++i) {
    size_t start=stick::u32(pack.data()+4+i*4), end=stick::u32(pack.data()+8+i*4);
    assert(start>=1216 && end>start && end<=pack.size());
    auto src=pack.data()+start; size_t size=stick::u32(src);
    std::vector<uint8_t> data(size);
    assert(stick::decode(src,end-start,data.data(),data.size()));
    stick::SpriteView view;
    assert(stick::parse(data.data(),data.size(),view));
    if(size>maxDecoded) maxDecoded=size;
    for(auto& a:view.acts) if(a.frames) {
      ++actions; frames+=a.frames;
      uint32_t duration=0;
      for(unsigned n=0;n<a.frames;++n) duration+=a.ms[n];
      assert(stick::frameAt(a,0)==0);
      assert(stick::frameAt(a,duration)==0);
      assert(stick::frameAt(a,duration-1)==a.frames-1);
    }
    assert(!stick::decode(src,end-start-1,data.data(),data.size()));
    assert(!stick::decode(src,end-start,data.data(),data.size()-1));
    // Whole-pair truncation and corrupt runs must never overrun the destination.
    assert(!stick::decode(src,end-start-2,data.data(),data.size()));
    assert(!stick::parse(data.data(),size-1,view));
    std::vector<uint8_t> bad(src,pack.data()+end); bad[4]=0;
    assert(!stick::decode(bad.data(),bad.size(),data.data(),data.size()));
    assert(stick::decode(src,end-start,data.data(),data.size()));
    data[4]=255; assert(!stick::parse(data.data(),size,view));
  }
  printf("PASS: 302 forms, %u actions, %u frames; truncation/overflow/corruption rejected; max decoded %zu bytes\n",actions,frames,maxDecoded);
}
