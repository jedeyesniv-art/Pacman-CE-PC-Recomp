#pragma once

#include <cstdint>

// Kernel handles. Each refers to a guest dispatcher object (KTHREAD, KEVENT, ...).
constexpr uint32_t CurrentThreadHandle = 0xFFFFFFFE;
constexpr uint32_t STATUS_INVALID_HANDLE = 0xC0000008;

uint32_t HandleCreate(uint32_t object);

// Guest object for a handle (the current-thread pseudo handle included), or 0 if it is not open.
uint32_t HandleObject(uint32_t handle);

bool HandleClose(uint32_t handle);
