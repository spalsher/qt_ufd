#!/usr/bin/env python3
"""Render the real firmware drawing functions into a host RGB565 framebuffer."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parent.parent
src=(root/'TOLL_DISPLAY/TOLL_DISPLAY.ino').read_text()
font=src[src.index('int ledCharIndex('):src.index('void panel3Vehicle(')]
small=src[src.index('void smallLine('):src.index('// Panel 3: vehicle number')]
render=src[src.index('void renderTransaction('):src.index('void legacyTransaction(')]
preamble=r'''
#include "Arduino.h"
#include "LedFont.h"
#include "PlateFont.h"
#include "DisplayPolicy.h"
#include <cassert>
#include <fstream>
#include <iostream>
const int SCREEN_W=64;
struct UfdPanel {static uint16_t color565(unsigned r,unsigned g,unsigned b){return ((r&248)<<8)|((g&252)<<3)|(b>>3);}};
struct Canvas {
 uint16_t pixels[96][64] = {}; int outside=0;
 void clearScreen(){memset(pixels,0,sizeof(pixels));outside=0;}
 void drawPixel(int x,int y,uint16_t c){if(x<0||x>=64||y<0||y>=96){outside++;return;}pixels[y][x]=c;}
 void fillRect(int x,int y,int w,int h,uint16_t c){for(int j=0;j<h;j++)for(int i=0;i<w;i++)drawPixel(x+i,y+j,c);}
 void drawFastHLine(int x,int y,int w,uint16_t c){fillRect(x,y,w,1,c);}
} canvas; auto scr=&canvas;
DisplayPolicy displayPolicy; String activePlate,activeFare,activeBalance;
bool scrollActive=false;void stopAnim(){}
'''
main=r'''
void save(const char*name){std::ofstream f(std::string(name)+".ppm",std::ios::binary);f<<"P6\n64 96\n255\n";for(auto &row:canvas.pixels)for(auto c:row){char rgb[]={char(((c>>11)&31)*255/31),char(((c>>5)&63)*255/63),char((c&31)*255/31)};f.write(rgb,3);}}
int main(){
 activePlate="AAA 000";activeFare="200";activeBalance="900";
 struct Sample{DisplayPolicy::Mode mode;const char*name;};
 Sample samples[]={{DisplayPolicy::WAITING,"waiting"},{DisplayPolicy::CHECKING,"checking"},{DisplayPolicy::PAID,"paid"},{DisplayPolicy::RFID_PAID,"rfid-paid"},{DisplayPolicy::ASSIST,"assist"},{DisplayPolicy::UNAVAILABLE,"expired"}};
 uint16_t plate[32][64]={};
 for(auto &s:samples){displayPolicy.mode=s.mode;renderTransaction();assert(canvas.outside==0);if(s.mode==DisplayPolicy::WAITING)memcpy(plate,canvas.pixels,sizeof(plate));else assert(!memcmp(plate,canvas.pixels,sizeof(plate)));save(s.name);}
 for(const char *money:{"0","1.50","12345678","99999.99"}){activeFare=money;activeBalance=money;displayPolicy.mode=DisplayPolicy::RFID_PAID;renderTransaction();assert(canvas.outside==0);}
 activePlate="WWWWWWWWWW";displayPolicy.mode=DisplayPolicy::WAITING;renderTransaction();assert(canvas.outside==0);
 activePlate="";activeFare="";displayPolicy.mode=DisplayPolicy::CHECKING;renderTransaction();assert(canvas.outside==0);
 std::cout<<"Firmware rendering: all checks passed; same plate across states; no clipping\n";
}
'''
out=root/'preview';out.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory() as tmp:
    Path(tmp,'Arduino.h').write_text('#include "arduino_shim.h"\n')
    test=Path(tmp,'render.cpp');test.write_text(preamble+font+small+render+main)
    exe=Path(tmp,'test')
    subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-I'+tmp,'-I'+str(root/'tests'),'-I'+str(root/'TOLL_DISPLAY'),str(test),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],cwd=out,check=True)
