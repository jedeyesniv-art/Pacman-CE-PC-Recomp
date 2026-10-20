// Null GPU: consumes the command ring buffer without drawing anything, so the game's D3D runtime sees its
// commands complete. Only the PM4 packets that write GPU registers or memory are executed (the game waits
// on what they write); the rest are skipped. It also fires the graphics interrupt callback for
// command-buffer interrupts and for a 60 Hz vertical blank.
//
// PM4 encoding and packet semantics follow Xenia's command processor (xenos.h, command_processor.cc).
// GPU registers are kept in guest memory at GpuRegisterBase + index * 4 (big-endian), where the game's
// CPU code reads and writes them directly.
#include "guest_thread.h"
#include "log.h"
#include "memory.h"
#include "kernel/memory_imports.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <atomic>
#include <chrono>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>

namespace
{

// Register indices.
constexpr uint32_t RegCpRbRptr = 0x01C4;
constexpr uint32_t RegCpRbWptr = 0x01C5;
constexpr uint32_t RegVblankStatus = 0x1951; // bit 0 is checked by the game's vblank handler (sub_8206ED50)
constexpr uint32_t RegisterCount = 0x8000;

// PM4 type-3 opcodes that are executed (values from Xenia's xenos.h); all others are skipped.
constexpr uint32_t PM4_REG_RMW = 0x21;
constexpr uint32_t PM4_INDIRECT_BUFFER_PFD = 0x37;
constexpr uint32_t PM4_WAIT_REG_MEM = 0x3C;
constexpr uint32_t PM4_MEM_WRITE = 0x3D;
constexpr uint32_t PM4_INDIRECT_BUFFER = 0x3F;
constexpr uint32_t PM4_COND_WRITE = 0x45;
constexpr uint32_t PM4_INTERRUPT = 0x54;
constexpr uint32_t PM4_EVENT_WRITE_SHD = 0x58;

// Interrupt sources passed to the guest callback (as in Xenia).
constexpr uint32_t InterruptVblank = 0;
constexpr uint32_t InterruptCommandBuffer = 1;

constexpr auto VblankInterval = std::chrono::microseconds(16667);
constexpr uint32_t InterruptStackSize = 0x40000;
constexpr int MaxIndirectDepth = 4;
constexpr uint32_t LoggedCalls = 16;

std::atomic<uint32_t> g_ringAddress;   // guest address of the ring buffer, 0 until initialised
std::atomic<uint32_t> g_ringDwords;
std::atomic<uint32_t> g_writebackAddress; // guest address the read pointer is written back to, or 0
std::atomic<uint32_t> g_interruptCallback;
std::atomic<uint32_t> g_interruptData;
std::atomic<bool> g_threadStarted;

// GPU-thread state.
uint32_t g_readIndex;
uint32_t g_vblankCount;
uint32_t g_opcodeCounts[128];
GuestThread g_interruptThread;

uint32_t ReadRegister(uint32_t index)
{
    return GuestLoadU32(GpuRegisterBase + (index % RegisterCount) * 4);
}

void WriteRegister(uint32_t index, uint32_t value)
{
    GuestStoreU32(GpuRegisterBase + (index % RegisterCount) * 4, value);
}

// The GPU's endian swap modes, selected by the low two bits of a memory address in a packet.
uint32_t GpuSwap(uint32_t value, uint32_t endian)
{
    switch (endian & 3)
    {
    case 1: return ((value >> 8) & 0x00FF00FF) | ((value << 8) & 0xFF00FF00); // 8 in 16
    case 2: return __builtin_bswap32(value);                                     // 8 in 32
    case 3: return (value >> 16) | (value << 16);                                // 16 in 32
    default: return value;
    }
}

uint32_t* GpuMemory(uint32_t physicalWithEndian)
{
    const uint32_t address = GuestPhysicalToVirtual(physicalWithEndian & ~3u);
    if (address == 0)
    {
        static std::atomic<uint32_t> calls;
        if (calls.fetch_add(1) < LoggedCalls)
            LogPrint("[gpu] access to unmapped physical address %08X", physicalWithEndian);
        return nullptr;
    }
    return reinterpret_cast<uint32_t*>(g_base + address);
}

// Memory writes and reads made by the GPU, with the packet's endian swap (host-order access as in Xenia).
void GpuStore(uint32_t physicalWithEndian, uint32_t value)
{
    if (uint32_t* p = GpuMemory(physicalWithEndian))
        *p = GpuSwap(value, physicalWithEndian);
}

uint32_t GpuLoad(uint32_t physicalWithEndian)
{
    const uint32_t* p = GpuMemory(physicalWithEndian);
    return p != nullptr ? GpuSwap(*p, physicalWithEndian) : 0;
}

void DispatchInterrupt(uint32_t source, uint32_t cpu)
{
    const uint32_t callback = g_interruptCallback.load();
    if (callback == 0)
        return;

    // The game's handler reads the current CPU from the PCR.
    g_base[g_interruptThread.pcr + 0x10C] = uint8_t(cpu);
    CallGuest(g_interruptThread, callback, { source, g_interruptData.load() });
}

// Reads packet dwords from the ring (wrapping) or from an indirect buffer.
struct PacketReader
{
    uint32_t address;
    uint32_t sizeDwords;
    uint32_t index;

    uint32_t Read()
    {
        const uint32_t value = GuestLoadU32(address + (index % sizeDwords) * 4);
        index++;
        return value;
    }
};

void ExecutePacket(PacketReader& reader, int depth);

void ExecuteIndirectBuffer(uint32_t physical, uint32_t dwords, int depth)
{
    const uint32_t address = GuestPhysicalToVirtual(physical);
    if (address == 0 || dwords == 0 || depth >= MaxIndirectDepth)
    {
        LogPrint("[gpu] skipping indirect buffer %08X (%u dwords, depth %d)", physical, dwords, depth);
        return;
    }

    PacketReader reader{ address, dwords, 0 };
    while (reader.index < dwords)
        ExecutePacket(reader, depth + 1);
}

bool Compare(uint32_t function, uint32_t value, uint32_t reference)
{
    switch (function & 7)
    {
    case 0: return false;
    case 1: return value < reference;
    case 2: return value <= reference;
    case 3: return value == reference;
    case 4: return value != reference;
    case 5: return value >= reference;
    case 6: return value > reference;
    default: return true;
    }
}

void ExecuteType3(PacketReader& reader, uint32_t opcode, uint32_t count, int depth)
{
    const uint32_t start = reader.index;
    if (g_opcodeCounts[opcode]++ == 0)
        LogPrint("[gpu] first PM4 type-3 packet with opcode 0x%02X (%u dwords)", opcode, count);

    switch (opcode)
    {
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD:
    {
        const uint32_t physical = reader.Read();
        const uint32_t dwords = reader.Read() & 0xFFFFF;
        ExecuteIndirectBuffer(physical, dwords, depth);
        break;
    }

    case PM4_MEM_WRITE:
    {
        const uint32_t address = reader.Read();
        for (uint32_t i = 1; i < count; i++)
            GpuStore((address & ~3u) + (i - 1) * 4 + (address & 3), reader.Read());
        break;
    }

    case PM4_EVENT_WRITE_SHD:
    {
        const uint32_t initiator = reader.Read();
        const uint32_t address = reader.Read();
        const uint32_t value = reader.Read();
        // Bit 31 of the initiator selects the GPU's counter instead of the packet's value.
        GpuStore(address, (initiator >> 31) & 1 ? g_vblankCount : value);
        break;
    }

    case PM4_COND_WRITE:
    {
        const uint32_t waitInfo = reader.Read();
        const uint32_t pollAddress = reader.Read();
        const uint32_t reference = reader.Read();
        const uint32_t mask = reader.Read();
        const uint32_t writeAddress = reader.Read();
        const uint32_t writeData = reader.Read();
        const uint32_t value = (waitInfo & 0x10) ? GpuLoad(pollAddress) : ReadRegister(pollAddress);
        if (Compare(waitInfo, value & mask, reference))
        {
            if (waitInfo & 0x100)
                GpuStore(writeAddress, writeData);
            else
                WriteRegister(writeAddress, writeData);
        }
        break;
    }

    case PM4_REG_RMW:
    {
        const uint32_t info = reader.Read();
        const uint32_t andMask = reader.Read();
        const uint32_t orMask = reader.Read();
        uint32_t value = ReadRegister(info & 0x1FFF);
        value &= (info >> 31) & 1 ? ReadRegister(andMask & 0x1FFF) : andMask;
        value |= (info >> 30) & 1 ? ReadRegister(orMask & 0x1FFF) : orMask;
        WriteRegister(info & 0x1FFF, value);
        break;
    }

    case PM4_INTERRUPT:
    {
        const uint32_t cpuMask = reader.Read();
        for (uint32_t cpu = 0; cpu < 6; cpu++)
        {
            if (cpuMask & (1u << cpu))
                DispatchInterrupt(InterruptCommandBuffer, cpu);
        }
        break;
    }

    case PM4_WAIT_REG_MEM:
        // Waits for a register or memory value. Nothing else runs on this GPU, so treat it as satisfied.
    default:
        break;
    }

    reader.index = start + count;
}

void ExecutePacket(PacketReader& reader, int depth)
{
    const uint32_t header = reader.Read();
    if (header == 0)
        return; // padding

    switch (header >> 30)
    {
    case 0: // register writes: count values to consecutive registers, or all to one
    {
        const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        const uint32_t base = header & 0x7FFF;
        const bool oneRegister = (header & 0x8000) != 0;
        for (uint32_t i = 0; i < count; i++)
            WriteRegister(oneRegister ? base : base + i, reader.Read());
        break;
    }
    case 1: // two register writes
    {
        const uint32_t first = header & 0x7FF;
        const uint32_t second = (header >> 11) & 0x7FF;
        WriteRegister(first, reader.Read());
        WriteRegister(second, reader.Read());
        break;
    }
    case 2: // no-op
        break;
    case 3:
        ExecuteType3(reader, (header >> 8) & 0x7F, ((header >> 16) & 0x3FFF) + 1, depth);
        break;
    }
}

// Executes everything between the read and write pointers and writes the read pointer back.
void ProcessRing()
{
    const uint32_t ring = g_ringAddress.load();
    const uint32_t dwords = g_ringDwords.load();
    if (ring == 0 || dwords == 0)
        return;

    const uint32_t writeIndex = ReadRegister(RegCpRbWptr) % dwords;
    if (writeIndex == g_readIndex)
        return;

    while (g_readIndex != writeIndex)
    {
        const uint32_t available = (writeIndex - g_readIndex + dwords) % dwords;
        PacketReader reader{ ring, dwords, g_readIndex };
        ExecutePacket(reader, 0);
        const uint32_t consumed = reader.index - g_readIndex;
        if (consumed > available)
        {
            LogPrint("[gpu] packet at ring index %u overran the write pointer (%u > %u dwords); skipping to it",
                g_readIndex, consumed, available);
            g_readIndex = writeIndex;
            break;
        }
        g_readIndex = reader.index % dwords;
    }

    WriteRegister(RegCpRbRptr, g_readIndex);
    if (const uint32_t writeback = g_writebackAddress.load())
        GuestStoreU32(writeback, g_readIndex);
}

void Vblank()
{
    g_vblankCount++;
    WriteRegister(RegVblankStatus, ReadRegister(RegVblankStatus) | 1);
    DispatchInterrupt(InterruptVblank, 2); // Xenia dispatches vblank on CPU 2
}

void GpuThread()
{
    LogPrint("[gpu] null GPU thread started");
    timeBeginPeriod(1);
    g_interruptThread = CreateGuestThread(InterruptStackSize, 0, true);

    auto nextVblank = std::chrono::steady_clock::now() + VblankInterval;
    for (;;)
    {
        ProcessRing();

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextVblank)
        {
            Vblank();
            nextVblank += VblankInterval;
            if (nextVblank < now)
                nextVblank = now + VblankInterval; // don't try to catch up after a stall
        }
        Sleep(1);
    }
}

void StartGpuThread()
{
    if (!g_threadStarted.exchange(true))
        std::thread(GpuThread).detach();
}

bool ShouldLog(std::atomic<uint32_t>& calls)
{
    return calls.fetch_add(1) < LoggedCalls;
}

} // namespace

// void VdInitializeRingBuffer(u32 physicalAddress, u32 sizeLog2): ring of 1 << (sizeLog2 + 3) bytes.
PPC_FUNC(__imp__VdInitializeRingBuffer)
{
    static std::atomic<uint32_t> calls;
    const uint32_t physical = ctx.r3.u32, sizeLog2 = ctx.r4.u32;
    const uint32_t address = GuestPhysicalToVirtual(physical);
    const uint32_t bytes = 1u << (sizeLog2 + 3);

    g_ringDwords = bytes / 4;
    g_ringAddress = address;
    if (ShouldLog(calls))
        LogPrint("[gpu] VdInitializeRingBuffer(physical=%08X, log2=%u) -> ring at %08X, 0x%X bytes", physical, sizeLog2, address, bytes);
    if (address == 0)
        LogPrint("[gpu] error: ring buffer physical address %08X is not mapped", physical);
    StartGpuThread();
}

// void VdEnableRingBufferRPtrWriteBack(u32 physicalAddress, u32 blockLog2)
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    static std::atomic<uint32_t> calls;
    const uint32_t physical = ctx.r3.u32;
    const uint32_t address = GuestPhysicalToVirtual(physical);
    g_writebackAddress = address;
    if (ShouldLog(calls))
        LogPrint("[gpu] VdEnableRingBufferRPtrWriteBack(physical=%08X, log2=%u) -> %08X", physical, ctx.r4.u32, address);
}

// void VdSetGraphicsInterruptCallback(u32 callback, u32 userData)
PPC_FUNC(__imp__VdSetGraphicsInterruptCallback)
{
    static std::atomic<uint32_t> calls;
    g_interruptData = ctx.r4.u32;
    g_interruptCallback = ctx.r3.u32;
    if (ShouldLog(calls))
        LogPrint("[gpu] VdSetGraphicsInterruptCallback(%08X, %08X)", ctx.r3.u32, ctx.r4.u32);
}
