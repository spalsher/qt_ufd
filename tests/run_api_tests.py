#!/usr/bin/env python3
"""Compile and execute the firmware's real API/validation functions against host I/O stubs.
Does not replace an ESP32 build or hardware tests.
"""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parent.parent
src=(root/'TOLL_DISPLAY/TOLL_DISPLAY.ino').read_text()
helpers=src[src.index('String urlDecode('):src.index('void sendHtml(')]
api=src[src.index('void jsonReply('):src.index('// NETWORK LOOP (core 0)')]
# Remove divider preceding networkLoop, harmless as comment but keep file tidy.
preamble=r'''
#include "arduino_shim.h"
#include "DisplayPolicy.h"
#include "UfdConfig.h"
#include <cassert>
#include <iostream>
uint32_t now=1000; uint32_t millis(){return now;}
struct {unsigned getFreeHeap(){return 100000;}} ESP;
struct NetworkClient {
 int status=0; std::string output;
 void printf(const char*,int s,const char*){status=s;}
 void print(const char*s){output+=s;}
 void print(const String&s){output+=s.c_str();}
 void print(unsigned n){output+=std::to_string(n);}
};
DisplayPolicy displayPolicy;
String activePlate,activeFare,activeBalance,activePassage,bootId="boot-1",lastPayload;
bool displayReady=true; int renders=0;
struct {void show(){renders++;}} panelObject; auto panels=&panelObject;
void startIdle(){displayPolicy.mode=DisplayPolicy::IDLE;}
void showTollPicture(const char*,int,const char*,const char*,bool){displayPolicy.mode=DisplayPolicy::CLOSED;}
void renderTransaction(){}
int FONT_S=0; int ledTextWidth(int,const String &s){return s.length()*5;}
'''
tests=r'''
NetworkClient send(const std::string &params){ NetworkClient c; applyFrame(c,String("POST /v1/frame?"+params+" HTTP/1.1")); return c; }
std::string frame(unsigned seq,const char*state="waiting",const char*boot="boot-1") {
 return "bootId="+std::string(boot)+"&sequence="+std::to_string(seq)+"&expiresAtMs=5000&passageId=car1&state="+state+"&plate=AAA%20000&fare=200";
}
int main(){
 assert(!validQuery("/v1/frame?state=paid&state=waiting"));
 assert(!validQuery("/v1/frame?%73tate=paid"));
 assert(validQuery("/v1/frame?plate=AAA%20000&fare=200"));
 assert(!amountValid("-1")); assert(!amountValid("1.999")); assert(!amountValid("1e3"));
 assert(amountValid("1234.50")); assert(!safeLabel("<script>",12,true));
 uint32_t n=0; assert(!unsignedValue("4294967296",n,UINT32_MAX));
 assert(unsignedValue("4294967295",n,UINT32_MAX) && n==UINT32_MAX);
 assert(send(frame(1,"waiting","old-boot")).status==409 && renders==0);
 assert(send(frame(1)).status==200 && renders==1);
 assert(activePlate=="AAA 000" && displayPolicy.mode==DisplayPolicy::WAITING);
 now=2000;
 assert(send(frame(1)).status==200 && renders==1 && displayPolicy.since==1000);
 assert(send(frame(1,"paid")).status==409 && renders==1);
 assert(send(frame(0)).status==409);
 assert(send(frame(2)+"&balance=-1").status==400);
 displayReady=false; assert(send(frame(2,"paid")).status==503 && displayPolicy.sequence==1);
 displayReady=true;
 now=6000; assert(send(frame(2,"paid")).status==409 && renders==1); // expired unseen request
 assert(send(frame(1)).status==200 && renders==1); // acknowledged duplicate never replays
 now=2000; assert(send(frame(2,"paid")).status==200 && renders==2);
 assert(send(frame(3,"closed")).status==200);
 assert(send(frame(4)).status==409);
 assert(send(frame(4,"open")).status==200);
 assert(send(frame(5,"rfid-paid")).status==400); // balance mandatory
 assert(send(frame(5,"rfid-paid")+"&balance=900").status==200);
 assert(displayPolicy.mode==DisplayPolicy::RFID_PAID);
 assert(send(frame(6,"typo")).status==400);
 assert(send(frame(6)+"&leaseMs=1").status==400);
 assert(displayPolicy.sequence==5);
 assert(send(frame(6)+"&unexpected=1").status==400);
 assert(send(frame(6)+"&leaseMs=300001").status==400);
 assert(!safeLabel(String(std::string("A\0B",3)),12,true));
 now=0xfffffff0u;
 auto rollover=frame(6);
 rollover.replace(rollover.find("expiresAtMs=5000"),16,"expiresAtMs=1000");
 assert(send(rollover).status==200); // valid deadline spanning uptime rollover

 std::cout<<"Firmware API: all checks passed\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
    test=Path(tmp)/'api.cpp'; test.write_text(preamble+helpers+api+tests)
    exe=Path(tmp)/'test'
    subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-I'+str(root/'tests'),'-I'+str(root/'TOLL_DISPLAY'),str(test),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
