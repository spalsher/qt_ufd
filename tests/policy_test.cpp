#include "../TOLL_DISPLAY/DisplayPolicy.h"
#include <cassert>
#include <iostream>
int main() {
  using P = DisplayPolicy;
  P p;
  assert(p.check(false,1,false,P::WAITING,false)==P::WRONG_BOOT);
  assert(p.check(true,0,false,P::WAITING,false)==P::STALE);
  p.commit(10,P::WAITING,100,120000);
  assert(!p.legacyIdleAllowed());
  assert(!p.expire(10100)); // fare must not vanish at the old 10s timeout
  assert(p.check(true,9,false,P::PAID,false)==P::STALE);
  assert(p.check(true,10,true,P::WAITING,false)==P::DUPLICATE);
  assert(p.check(true,10,false,P::PAID,false)==P::CONFLICT);
  assert(p.check(true,11,false,P::PAID,false)==P::APPLY);
  assert(p.expire(120100) && p.mode==P::UNAVAILABLE);
  p.commit(11,P::PAID,200000,120000);
  assert(!p.expire(205999));
  assert(p.expire(206000) && p.mode==P::IDLE);
  assert(!p.legacyIdleAllowed()); // old clients stay blocked even after completion
  assert(p.check(true,11,true,P::PAID,false)==P::DUPLICATE); // must not replay
  p.commit(12,P::CLOSED,300000,120000);
  assert(!p.expire(900000));
  assert(p.check(true,13,false,P::WAITING,false)==P::LOCKED);
  assert(p.check(true,13,false,P::IDLE,true)==P::APPLY);
  p.commit(13,P::RFID_PAID,0xfffffff0u,120000);
  assert(!p.expire(0x100u));
  assert(p.expire(5984u)); // six seconds through millis wrap
  std::cout << "Display policy: all checks passed\n";
}
