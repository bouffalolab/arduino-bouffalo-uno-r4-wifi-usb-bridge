// Host-only update backend for AT framing tests; no Flash or image validation.
#include <BLUpdate.h>
#include "Arduino.h"

BLUpdateClass BLUpdate;

bool BLUpdateClass::begin(const uint8_t *header, size_t length) {
    if (!header || length != 512 || state_ == State::Receiving || state_ == State::Verified) return false;
    state_ = State::Receiving;
    written_ = 0;
    size_ = 512;
    return true;
}
bool BLUpdateClass::write(uint32_t offset, const uint8_t *data, size_t length) {
    if (state_ != State::Receiving || !data || !length || offset != written_ || length > size_ - written_) return false;
    written_ += length;
    return true;
}
bool BLUpdateClass::finish() {
    if (state_ != State::Receiving || written_ != size_) return false;
    state_ = State::Verified;
    return true;
}
bool BLUpdateClass::commit() {
    if (state_ != State::Verified) return false;
    state_ = State::Committed;
    return true;
}
void BLUpdateClass::abort() { state_ = State::Idle; written_ = size_ = 0; }
extern "C" void bl_sys_reset_por(void) { resetToBootloader(); }
