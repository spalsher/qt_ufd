#pragma once
#include <string>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
class String {
  std::string v;
public:
  String() {} String(const char* x):v(x?x:""){} String(const std::string& x):v(x){}
  String(unsigned long x):v(std::to_string(x)){} String(unsigned x):v(std::to_string(x)){}
  String(int x):v(std::to_string(x)){}
  void toUpperCase(){for(auto &c:v)c=std::toupper((unsigned char)c);}
  void trim(){auto a=v.find_first_not_of(" \r\n\t");if(a==std::string::npos){v.clear();return;}auto b=v.find_last_not_of(" \r\n\t");v=v.substr(a,b-a+1);}
  void reserve(unsigned n){v.reserve(n);}
  void replace(const String&a,const String&b){size_t p=0;while((p=v.find(a.v,p))!=std::string::npos){v.replace(p,a.v.size(),b.v);p+=b.v.size();}}
  unsigned length() const{return v.size();} const char* c_str()const{return v.c_str();}
  char operator[](unsigned i)const{return v[i];}
  String substring(unsigned a, unsigned b=UINT32_MAX)const {return v.substr(std::min<size_t>(a,v.size()),b==UINT32_MAX?std::string::npos:b-a);}
  int indexOf(const String& s, unsigned at=0)const{auto p=v.find(s.v,at);return p==std::string::npos?-1:int(p);}
  int indexOf(char c,unsigned at=0)const{auto p=v.find(c,at);return p==std::string::npos?-1:int(p);}
  String& operator+=(const String& s){v+=s.v;return *this;}
  String& operator+=(char c){v+=c;return *this;}
  friend String operator+(const String&a,const String&b){return a.v+b.v;}
  friend bool operator==(const String&a,const String&b){return a.v==b.v;}
  friend bool operator!=(const String&a,const String&b){return !(a==b);}
};
