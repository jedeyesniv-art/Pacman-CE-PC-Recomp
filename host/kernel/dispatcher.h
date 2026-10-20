#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

// Xbox dispatcher objects (KEVENT, KSEMAPHORE, KTHREAD) live in guest memory and start with an
// X_DISPATCHER_HEADER (layout as in Xenia): u8 type, u8 absolute, u8 size, u8 inserted,
// big-endian s32 signal state at +4, then a LIST_ENTRY wait list. The signal state in guest memory is the
// single source of truth; the host changes it and checks waits under one lock.
constexpr uint8_t DispatcherNotificationEvent = 0;   // manual reset
constexpr uint8_t DispatcherSynchronizationEvent = 1; // auto reset
constexpr uint8_t DispatcherSemaphore = 5;
constexpr uint8_t DispatcherThread = 6;
constexpr uint32_t DispatcherHeaderSize = 16;

constexpr uint32_t STATUS_TIMEOUT = 0x00000102;

using Deadline = std::optional<std::chrono::steady_clock::time_point>; // nullopt = wait forever

// Converts an Xbox timeout (guest pointer to a big-endian LARGE_INTEGER in 100 ns units: negative is
// relative, positive is an absolute system time; a null pointer means infinite) into a deadline.
Deadline DeadlineFromTimeout(uint32_t timeoutPtr);

// Current system time in 100 ns units since 1601, the clock KeQuerySystemTime reports.
uint64_t SystemTime100ns();

void DispatcherInit(uint32_t object, uint8_t type, int32_t signalState);

// Sets the signal state and wakes waiters; returns the previous state.
int32_t DispatcherSetState(uint32_t object, int32_t state);

// Adds to the signal state (semaphore release) and wakes waiters; returns the previous state.
int32_t DispatcherAddState(uint32_t object, int32_t delta);

// Waits until any (returns its index) or all (returns 0) of the objects are signaled, or returns
// STATUS_TIMEOUT at the deadline. A satisfied wait consumes auto-reset events and semaphore counts.
uint32_t DispatcherWait(const uint32_t* objects, uint32_t count, bool waitAll, Deadline deadline);
