#include "zonewriter.h"
#include <algorithm>
#include <stdexcept>

namespace zone {

void ZoneWriter::writeUntracked(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    bytes_.insert(bytes_.end(), b, b + n);
}

void ZoneWriter::setIndex(int block) {
    // Positions are kept per block, so switching just changes which one advances.
    cur_ = block;
}

void ZoneWriter::push(int block) {
    if (block < 0 || block >= BLOCK_COUNT) throw std::out_of_range("zone block");
    Saved s;
    s.block = cur_;
    setIndex(block);
    s.pos = pos_[cur_];  // the loader records the position of the block being entered
    stack_.push_back(s);
}

void ZoneWriter::pop() {
    if (stack_.empty()) throw std::logic_error("zone stream stack underflow");
    Saved s = stack_.back();
    stack_.pop_back();
    if (cur_ == BLOCK_TEMP) pos_[BLOCK_TEMP] = s.pos;  // TEMP is reused after each asset
    setIndex(s.block);
}

uint32_t ZoneWriter::align(uint32_t mask) {
    pos_[cur_] = (pos_[cur_] + mask) & ~mask;
    high_[cur_] = std::max(high_[cur_], pos_[cur_]);
    return pos_[cur_];
}

uint64_t ZoneWriter::insertPointer() {
    int prev = cur_;
    setIndex(BLOCK_VIRTUAL);
    uint32_t slot = align(kAlign8);
    advance(8);  // the slot is reserved in memory but has no bytes in the file
    setIndex(prev);
    return offsetPointer(BLOCK_VIRTUAL, slot);
}

void ZoneWriter::advance(size_t n) {
    pos_[cur_] += static_cast<uint32_t>(n);
    high_[cur_] = std::max(high_[cur_], pos_[cur_]);
}

void ZoneWriter::write(const void* p, size_t n) {
    // Load_Stream zero-fills RUNTIME memory instead of reading the file, so it has no bytes.
    if (cur_ != BLOCK_RUNTIME) writeUntracked(p, n);
    advance(n);
}

void ZoneWriter::zeros(size_t n) {
    if (cur_ != BLOCK_RUNTIME) bytes_.insert(bytes_.end(), n, 0);
    advance(n);
}

void ZoneWriter::cstr(const std::string& s) {
    write(s.data(), s.size());
    u8(0);
}

void ZoneWriter::xstring(const std::string& s) {
    align(kAlign1);
    cstr(s);
}

}  // namespace zone
