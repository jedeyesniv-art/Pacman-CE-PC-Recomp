// Kept separate from the PPC headers: XenonUtils' xex.h brings its own Xbox type definitions.
#include "loader.h"
#include "log.h"
#include "memory.h"

#include <image.h>
#include <xex.h>
#include <ppc_config.h>

#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Ordinal -> export name tables, defined in XenonUtils' xex.cpp.
extern std::unordered_map<size_t, const char*> XamExports;
extern std::unordered_map<size_t, const char*> XboxKernelExports;

// Size of the zeroed block each data import gets until it has a real implementation.
constexpr uint32_t DataImportBlockSize = 0x1000;

// XenonUtils replaces each import thunk with this code (host byte order) and records its name in image.symbols.
static constexpr uint32_t ThunkCode[4] = { 0x00000060, 0x00000060, 0x00000060, 0x2000804E };

static bool IsThunk(uint32_t address)
{
    return std::memcmp(g_base + address, ThunkCode, sizeof(ThunkCode)) == 0;
}

// Variable imports (type-0 records) are left by XenonUtils as byte-swapped records. Each one is
// either the pointer slot of a function import, which gets the thunk's address, or a data export,
// which gets a zeroed guest block.
static void ResolveVariableImports(const uint8_t* xex, const Image& image)
{
    auto* header = static_cast<const Xex2ImportHeader*>(getOptHeaderPtr(xex, XEX_HEADER_IMPORT_LIBRARIES));
    if (header == nullptr)
    {
        LogPrint("no import libraries");
        return;
    }

    std::vector<std::string_view> libraryNames;
    auto* stringTable = reinterpret_cast<const char*>(header + 1);
    size_t stringOffset = 0;
    for (size_t i = 0; i < header->numImports; i++)
    {
        libraryNames.emplace_back(stringTable + stringOffset);
        stringOffset += ((libraryNames.back().length() + 1) + 3) & ~3;
    }

    size_t thunks = 0, functionSlots = 0, dataImports = 0, unknown = 0;
    auto* library = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const char*>(header) + sizeof(Xex2ImportHeader) + header->sizeOfStringTable);
    for (size_t i = 0; i < libraryNames.size(); i++)
    {
        static const std::unordered_map<size_t, const char*> NoExports;
        const auto* exports = &NoExports;
        if (libraryNames[i] == "xam.xex")
            exports = &XamExports;
        else if (libraryNames[i] == "xboxkrnl.exe")
            exports = &XboxKernelExports;

        auto* descriptors = reinterpret_cast<const Xex2ImportDescriptor*>(library + 1);

        // Thunk address for each named function import of this library.
        std::unordered_map<std::string, uint32_t> thunkByName;
        for (size_t im = 0; im < library->numberOfImports; im++)
        {
            uint32_t slot = descriptors[im].firstThunk;
            if (!IsThunk(slot))
                continue;

            thunks++;
            auto symbol = image.symbols.find(slot);
            if (symbol != image.symbols.end() && symbol->address == slot)
                thunkByName.emplace(symbol->name, slot);
        }

        for (size_t im = 0; im < library->numberOfImports; im++)
        {
            uint32_t slot = descriptors[im].firstThunk;
            if (IsThunk(slot))
                continue;

            uint32_t record = *reinterpret_cast<uint32_t*>(g_base + slot); // host order, see above
            uint32_t ordinal = record & 0xFFFF;
            auto name = exports->find(ordinal);
            if (name == exports->end())
            {
                uint32_t block = GuestAlloc(DataImportBlockSize);
                GuestStoreU32(slot, block);
                LogPrint("import: %.*s ordinal %u has no known name (record 0x%08X at 0x%08X) -> zeroed block 0x%08X",
                    int(libraryNames[i].size()), libraryNames[i].data(), ordinal, record, slot, block);
                unknown++;
                continue;
            }

            auto thunk = thunkByName.find(name->second);
            if (thunk != thunkByName.end())
            {
                GuestStoreU32(slot, thunk->second);
                functionSlots++;
            }
            else
            {
                uint32_t block = GuestAlloc(DataImportBlockSize);
                GuestStoreU32(slot, block);
                LogPrint("import: data %s (%.*s) at 0x%08X -> zeroed block 0x%08X",
                    name->second, int(libraryNames[i].size()), libraryNames[i].data(), slot, block);
                dataImports++;
            }
        }

        library = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const char*>(library + 1) + library->numberOfImports * sizeof(Xex2ImportDescriptor));
    }

    LogPrint("imports: %zu function thunks, %zu function pointer slots, %zu data imports, %zu unknown",
        thunks, functionSlots, dataImports, unknown);
}

bool LoadXex(const char* path, XexInfo& info)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        LogPrint("error: could not open %s", path);
        return false;
    }
    std::vector<uint8_t> xex((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    Image image = Image::ParseImage(xex.data(), xex.size());
    if (image.data == nullptr || image.size == 0)
    {
        LogPrint("error: XenonUtils could not parse %s", path);
        return false;
    }

    if (image.base != PPC_IMAGE_BASE || image.size > PPC_IMAGE_SIZE)
    {
        LogPrint("error: image 0x%zX+0x%X does not match the recompiled image 0x%llX+0x%llX - recompile?",
            image.base, image.size, PPC_IMAGE_BASE, PPC_IMAGE_SIZE);
        return false;
    }

    std::memcpy(g_base + image.base, image.data.get(), image.size);

    info.entryPoint = uint32_t(image.entry_point);
    info.imageBase = uint32_t(image.base);
    info.imageSize = image.size;
    auto* stackSize = static_cast<const be<uint32_t>*>(getOptHeaderPtr(xex.data(), XEX_HEADER_DEFAULT_STACK_SIZE));
    info.stackSize = stackSize != nullptr ? uint32_t(*stackSize) : 0;

    LogPrint("loaded %s: image 0x%08X+0x%X, entry 0x%08X, stack size 0x%X%s", path, info.imageBase, info.imageSize,
        info.entryPoint, info.stackSize, info.stackSize == 0 ? " (not in header)" : "");

    if (getOptHeaderPtr(xex.data(), XEX_HEADER_TLS_INFO) != nullptr)
        LogPrint("warning: XEX has a TLS info header; static TLS is not set up yet");

    ResolveVariableImports(xex.data(), image);
    return true;
}
