#pragma once

#include <cstdint>

struct XexInfo
{
    uint32_t entryPoint;
    uint32_t imageBase;
    uint32_t imageSize;
    uint32_t stackSize; // from the XEX header, 0 if absent
    uint32_t tlsSlots;  // dynamic TLS slots from the XEX TLS header, 0 if absent
};

// Loads the XEX into guest memory and resolves its variable imports.
bool LoadXex(const char* path, XexInfo& info);
