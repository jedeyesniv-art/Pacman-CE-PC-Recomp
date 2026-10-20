#pragma once

#include <ppc_config.h>
#include <ppc_context.h>

#include <cstdint>
#include <initializer_list>

// A guest execution context: register state plus the guest stack and the PCR its r13 points at.
struct GuestThread
{
    PPCContext* ctx;
    uint32_t stackLow;
    uint32_t stackHigh;
    uint32_t pcr;
    uint32_t threadObject; // zeroed placeholder KTHREAD
};

// Creates a context with its own guarded stack, PCR and thread object. cpu goes in the PCR's current-CPU
// byte; dpcActive marks interrupt (DPC) contexts, which the game checks before touching per-thread state.
GuestThread CreateGuestThread(uint32_t stackSize, uint8_t cpu, bool dpcActive);

// Runs a guest function to completion on the calling host thread, with up to 8 args in r3, r4, ...
// The stack pointer is reset to the top of the context's stack first. Returns r3.
uint32_t CallGuest(GuestThread& thread, uint32_t function, std::initializer_list<uint32_t> args);

// The context the calling host thread is running guest code with, or null.
PPCContext* CurrentGuestContext();
