#pragma once

#include <cstdint>

// Physical address of a guest virtual address in one of the physical windows, as MmGetPhysicalAddress
// (and D3D's inline copy of it) computes it.
uint32_t GuestVirtualToPhysical(uint32_t address);

// Guest virtual address of an allocated physical address (as the GPU sees it), or 0 if no physical
// window has it allocated. Physical windows are not aliased here, so the allocation's window is used.
uint32_t GuestPhysicalToVirtual(uint32_t physical);
