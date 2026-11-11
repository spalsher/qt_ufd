#pragma once
#include <stdint.h>

// No Arduino dependency: the exact policy is also exercised by host tests.
struct DisplayPolicy {
  enum Mode { IDLE, WAITING, CHECKING, PAID, RFID_PAID, ASSIST, CLOSED, UNAVAILABLE };
  enum Decision { APPLY, DUPLICATE, CONFLICT, STALE, WRONG_BOOT, LOCKED };
  Mode mode = IDLE;
  uint32_t sequence = 0, since = 0, leaseMs = 120000;
  bool controlled = false;

  Decision check(bool bootMatches, uint32_t seq, bool samePayload, Mode next, bool explicitOpen) const {
    if (!bootMatches) return WRONG_BOOT;
    if (seq == 0 || seq < sequence) return STALE;
    if (seq == sequence) return samePayload ? DUPLICATE : CONFLICT;
    if (mode == CLOSED && next != CLOSED && !explicitOpen) return LOCKED;
    return APPLY;
  }
  void commit(uint32_t seq, Mode next, uint32_t now, uint32_t lease) {
    controlled = true; sequence = seq; mode = next; since = now; leaseMs = lease;
  }
  bool expire(uint32_t now) {
    uint32_t age = now - since; // unsigned subtraction handles millis() rollover
    if ((mode == PAID || mode == RFID_PAID) && age >= 6000) { mode = IDLE; return true; }
    if ((mode == WAITING || mode == CHECKING || mode == ASSIST) && age >= leaseMs) {
      mode = UNAVAILABLE; return true;
    }
    return false;
  }
  bool legacyIdleAllowed() const { return !controlled && mode == IDLE; }
};
