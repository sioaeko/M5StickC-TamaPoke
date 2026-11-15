#pragma once
#include <fstream>
#include <memory>
#include <string>
struct File {
  std::shared_ptr<std::ifstream> f;
  explicit operator bool() const{return f&&f->good();}
  size_t read(uint8_t* p,size_t n){f->read((char*)p,n);return f->gcount();}
  bool seek(size_t p){f->clear();f->seekg(p);return bool(*f);}
  size_t size(){auto pos=f->tellg();f->seekg(0,std::ios::end);auto n=f->tellg();f->seekg(pos);return size_t(n);}
};
struct FsMock {
  bool begin(bool){return true;}
  File open(const char* path,const char*){return {std::make_shared<std::ifstream>(std::string("ports/stickc-plus2/data")+path,std::ios::binary)};}
  bool exists(const char* p){return bool(open(p,"r"));}
};
static FsMock LittleFS;
