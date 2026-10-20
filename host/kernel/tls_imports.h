#pragma once

#include <cstdint>

// Most dynamic TLS slots any title can ask for.
constexpr uint32_t MaxTlsSlots = 1024;

// Sets how many slots KeTlsAlloc hands out (the XEX TLS header's slot count).
void TlsInit(uint32_t slotCount);
