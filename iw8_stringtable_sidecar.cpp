// IW8 loose string_table sidecar - overlay/cyclic expanded-pool-index build
// Build as x64 DLL with MSVC, C++17+.
// Intended for local/offline mod testing alongside the current iw8-mod version.dll.
//
// Why this version exists:
// Older sidecar builds called DB_FindXAssetHeader from a background thread. In IW8/iw8-mod
// that can crash because the asset DB / version.dll hook path is not safe from arbitrary
// injected threads. This build instead hooks the existing DB_FindXAssetHeader entry and only
// patches stringtables when the game itself asks for them.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdarg>
#include <cstring>
#include <limits>
#include <memory>

namespace fs = std::filesystem;

namespace iw8st {

constexpr std::uintptr_t DB_FIND_XASSETHEADER_CALLSITE_RVA_141 = 0x327B578; // ModernWarfare.exe.json key EB6CDC20 match RVA; signature uses Add(1).Rip() to reach the real function
constexpr int ASSET_TYPE_STRINGTABLE = 0x36;

struct StringTable {
    const char* name;           // 0x00
    int columnCount;            // 0x08
    int rowCount;               // 0x0C
    int uniqueCellCount;        // 0x10
    std::uint16_t* cellIndices; // 0x18, row-major indices into unique string pool
    int* hashes;                // 0x20, one hash per unique string
    const char** strings;       // 0x28, unique string pool
};

union XAssetHeader {
    void* data;
    StringTable* stringTable;
};

using DB_FindXAssetHeader_t = XAssetHeader(__fastcall*)(int type, const char* name, int allowCreateDefault);

struct CsvTable {
    std::vector<std::vector<std::string>> rows;
    std::size_t cols{};
};

struct BlueprintAttachmentPatch {
    std::vector<std::string> variantIds;
    std::vector<std::string> attachments;
    int sourceColumn{};
};

using BlueprintAttachmentMap = std::unordered_map<std::string, std::vector<BlueprintAttachmentPatch>>;

struct OwnedTable {
    std::string assetName;
    std::vector<std::string> stringStorage;
    std::vector<const char*> stringPointers;
    std::vector<int> hashes;
    std::vector<std::uint16_t> cellIndices;
    fs::file_time_type lastWrite{};
    StringTable* target{};
};

static HMODULE g_module{};
static DB_FindXAssetHeader_t g_previousDBFind{};  // existing iw8-mod/version hook or original function
static void* g_dbFindEntry{};                     // ModernWarfare.exe + DB_FIND_XASSETHEADER_RVA_141
static std::mutex g_logMutex;
static std::mutex g_fileProbeMutex;
static std::mutex g_replaceMutex;
static std::unordered_map<std::string, OwnedTable*> g_active;
static std::unordered_map<std::string, StringTable*> g_lastGoodTableByAsset;
static std::unordered_map<std::string, bool> g_loggedInvalidShapeByAsset;
static std::unordered_map<std::string, bool> g_loggedFallbackByAsset;
static std::unordered_map<std::string, bool> g_loggedScriptProbeByAsset;
static std::unordered_map<std::string, bool> g_loggedFileProbeByPath;
static std::vector<std::unique_ptr<OwnedTable>> g_ownedPool;
static wchar_t g_logPath[MAX_PATH]{};
static fs::path g_sidecarDir;
static fs::path g_gameRoot;
static std::vector<fs::path> g_assetRoots;
static bool g_hookInstalled{};
static bool g_hookInstallFailed{};
static bool g_fileProbeHookInstalled{};
static thread_local bool g_inHook{};
static thread_local bool g_inFileProbeHook{};

using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static CreateFileW_t g_realCreateFileW = ::CreateFileW;
static CreateFileA_t g_realCreateFileA = ::CreateFileA;

std::string narrow(const std::wstring& ws) {
    if (ws.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), out.data(), size, nullptr, nullptr);
    return out;
}

void initLogPath(HMODULE module) {
    wchar_t modulePath[MAX_PATH]{};
    if (module && GetModuleFileNameW(module, modulePath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(modulePath, L'\\');
        if (slash) {
            *(slash + 1) = L'\0';
            lstrcpynW(g_logPath, modulePath, MAX_PATH);
            lstrcatW(g_logPath, L"iw8_stringtable_sidecar.log");
            return;
        }
    }

    lstrcpynW(g_logPath, L"C:\\Games\\COD\\iw8_stringtable_sidecar.log", MAX_PATH);
}

void appendLogLine(const char* line) {
    OutputDebugStringA(line);
    OutputDebugStringA("\n");

    if (!g_logPath[0]) return;

    HANDLE file = CreateFileW(
        g_logPath,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (file == INVALID_HANDLE_VALUE) return;

    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
    const char newline[] = "\r\n";
    WriteFile(file, newline, 2, &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

void log(const char* fmt, ...) {
    char message[2048]{};

    va_list args;
    va_start(args, fmt);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME st{};
    GetLocalTime(&st);

    char line[2300]{};
    snprintf(line, sizeof(line), "[%02u:%02u:%02u] %s",
        static_cast<unsigned>(st.wHour),
        static_cast<unsigned>(st.wMinute),
        static_cast<unsigned>(st.wSecond),
        message);

    std::lock_guard<std::mutex> lock(g_logMutex);
    appendLogLine(line);
}

std::string normalizeAssetName(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    while (!s.empty() && (s.front() == '/' || s.front() == '.')) s.erase(s.begin());
    return s;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(std::string s) {
    auto first = s.begin();
    while (first != s.end() && std::isspace(static_cast<unsigned char>(*first))) ++first;

    auto last = s.end();
    while (last != first && std::isspace(static_cast<unsigned char>(*(last - 1)))) --last;

    return std::string(first, last);
}

bool isEmptyOrNoneValue(const std::string& value) {
    const std::string lower = toLower(trim(value));
    return lower.empty() || lower == "none" || lower == "null";
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool containsAnyScriptProbeToken(const std::string& lowerPath) {
    return lowerPath.find(".gscbin") != std::string::npos ||
           lowerPath.find("gametypes/arena") != std::string::npos ||
           lowerPath.find("validation") != std::string::npos;
}

bool shouldLogFileProbePath(const std::string& rawPath) {
    if (rawPath.empty()) return false;

    const std::string path = toLower(normalizeAssetName(rawPath));
    if (path.find("iw8_stringtable_sidecar.log") != std::string::npos) return false;
    return containsAnyScriptProbeToken(path);
}

void logFileProbeResult(const char* apiName, const std::string& rawPath, DWORD desiredAccess, DWORD creationDisposition, HANDLE result, DWORD error) {
    if (!shouldLogFileProbePath(rawPath)) return;

    const std::string path = normalizeAssetName(rawPath);
    const std::string key = std::string(apiName) + ":" + toLower(path) + ":" + std::to_string(creationDisposition);

    std::lock_guard<std::mutex> lock(g_fileProbeMutex);
    if (g_loggedFileProbeByPath[key]) return;
    if (g_loggedFileProbeByPath.size() >= 250) return;
    g_loggedFileProbeByPath[key] = true;

    log("File probe %s: path='%s' access=0x%08lX disposition=%lu result=%p error=%lu",
        apiName,
        path.c_str(),
        static_cast<unsigned long>(desiredAccess),
        static_cast<unsigned long>(creationDisposition),
        result,
        static_cast<unsigned long>(error));
}

HANDLE WINAPI hkCreateFileW(LPCWSTR fileName, DWORD desiredAccess, DWORD shareMode, LPSECURITY_ATTRIBUTES securityAttributes, DWORD creationDisposition, DWORD flagsAndAttributes, HANDLE templateFile) {
    if (g_inFileProbeHook) {
        return g_realCreateFileW(fileName, desiredAccess, shareMode, securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
    }

    g_inFileProbeHook = true;
    HANDLE result = g_realCreateFileW(fileName, desiredAccess, shareMode, securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
    const DWORD error = GetLastError();

    if (fileName) {
        logFileProbeResult("CreateFileW", narrow(fileName), desiredAccess, creationDisposition, result, error);
    }

    SetLastError(error);
    g_inFileProbeHook = false;
    return result;
}

HANDLE WINAPI hkCreateFileA(LPCSTR fileName, DWORD desiredAccess, DWORD shareMode, LPSECURITY_ATTRIBUTES securityAttributes, DWORD creationDisposition, DWORD flagsAndAttributes, HANDLE templateFile) {
    if (g_inFileProbeHook) {
        return g_realCreateFileA(fileName, desiredAccess, shareMode, securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
    }

    g_inFileProbeHook = true;
    HANDLE result = g_realCreateFileA(fileName, desiredAccess, shareMode, securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
    const DWORD error = GetLastError();

    if (fileName) {
        logFileProbeResult("CreateFileA", fileName, desiredAccess, creationDisposition, result, error);
    }

    SetLastError(error);
    g_inFileProbeHook = false;
    return result;
}

bool shouldProbeModule(const wchar_t* moduleName) {
    if (!moduleName || !*moduleName) return false;

    const std::string name = toLower(narrow(moduleName));
    return name == "modernwarfare.exe" ||
           name == "version.dll" ||
           name == "ucrtbase.dll" ||
           name == "msvcp140.dll" ||
           name == "vcruntime140.dll" ||
           name == "vcruntime140_1.dll" ||
           name == "concrt140.dll";
}

int patchCreateFileImportsForModule(HMODULE module, const wchar_t* moduleName) {
    if (!module || module == g_module) return 0;

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || !imports.Size) return 0;

    int patched = 0;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        auto* lookupThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk : descriptor->FirstThunk));
        auto* importThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);

        for (; lookupThunk->u1.AddressOfData && importThunk->u1.Function; ++lookupThunk, ++importThunk) {
            if (lookupThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;

            auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + lookupThunk->u1.AddressOfData);
            const char* importName = reinterpret_cast<const char*>(importByName->Name);
            void* replacement = nullptr;

            if (strcmp(importName, "CreateFileW") == 0) {
                const auto existing = reinterpret_cast<CreateFileW_t>(importThunk->u1.Function);
                if (existing == &hkCreateFileW) continue;
                g_realCreateFileW = existing;
                replacement = reinterpret_cast<void*>(&hkCreateFileW);
            } else if (strcmp(importName, "CreateFileA") == 0) {
                const auto existing = reinterpret_cast<CreateFileA_t>(importThunk->u1.Function);
                if (existing == &hkCreateFileA) continue;
                g_realCreateFileA = existing;
                replacement = reinterpret_cast<void*>(&hkCreateFileA);
            }

            if (!replacement) continue;

            DWORD oldProtect = 0;
            if (!VirtualProtect(&importThunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                log("File probe IAT VirtualProtect failed for module '%s' import '%s': %lu",
                    narrow(moduleName).c_str(),
                    importName,
                    GetLastError());
                continue;
            }

            importThunk->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
            FlushInstructionCache(GetCurrentProcess(), &importThunk->u1.Function, sizeof(void*));

            DWORD ignored = 0;
            VirtualProtect(&importThunk->u1.Function, sizeof(void*), oldProtect, &ignored);
            ++patched;
        }
    }

    if (patched > 0) {
        log("File probe IAT hooks installed for module '%s': patchedImports=%d", narrow(moduleName).c_str(), patched);
    }

    return patched;
}

bool installFileProbeHooks() {
    if (g_fileProbeHookInstalled) return true;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        log("File probe module snapshot failed: %lu", GetLastError());
        return false;
    }

    int modulesSeen = 0;
    int importsPatched = 0;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (!shouldProbeModule(entry.szModule)) continue;
            ++modulesSeen;
            importsPatched += patchCreateFileImportsForModule(entry.hModule, entry.szModule);
        } while (Module32NextW(snapshot, &entry));
    } else {
        log("File probe Module32FirstW failed: %lu", GetLastError());
    }

    CloseHandle(snapshot);

    g_fileProbeHookInstalled = importsPatched > 0;
    log("File probe hook summary: modulesSeen=%d importsPatched=%d installed=%d",
        modulesSeen,
        importsPatched,
        g_fileProbeHookInstalled ? 1 : 0);
    return g_fileProbeHookInstalled;
}

bool isReadableMemoryProtection(DWORD protect) {
    if (protect & PAGE_GUARD) return false;
    if (protect & PAGE_NOACCESS) return false;

    const DWORD baseProtect = protect & 0xFF;
    return baseProtect == PAGE_READONLY ||
           baseProtect == PAGE_READWRITE ||
           baseProtect == PAGE_WRITECOPY ||
           baseProtect == PAGE_EXECUTE_READ ||
           baseProtect == PAGE_EXECUTE_READWRITE ||
           baseProtect == PAGE_EXECUTE_WRITECOPY;
}

bool bytesAreTakefistsOnePattern(const std::uint8_t* p) {
    return p[0] == 0x16 &&
           p[1] == 0x01 &&
           p[2] == 0x38 &&
           p[3] == 0x2A &&
           p[4] == 0xC6 &&
           p[5] == 0x00 &&
           p[6] == 0x00;
}

bool tryPatchTakefistsPatternAt(std::uint8_t* address) {
    std::uint8_t current[7]{};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), address, current, sizeof(current), &bytesRead) || bytesRead != sizeof(current)) {
        return false;
    }

    if (!bytesAreTakefistsOnePattern(current)) {
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(address + 1, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        log("Takefists memory patch VirtualProtect failed at %p: %lu", address, GetLastError());
        return false;
    }

    address[1] = 0x00;
    FlushInstructionCache(GetCurrentProcess(), address, 7);

    DWORD ignored = 0;
    VirtualProtect(address + 1, 1, oldProtect, &ignored);
    return true;
}

struct TakefistsScanStats {
    std::uint64_t scannedRegions{};
    std::uint64_t scannedBytes{};
    std::uint64_t skippedLargeRegions{};
    std::uint64_t readFailures{};
    int patched{};
};

int scanTakefistsRegion(const MEMORY_BASIC_INFORMATION& mbi, TakefistsScanStats& stats) {
    static constexpr std::size_t patternLen = 7;
    static constexpr std::size_t overlapLen = patternLen - 1;
    static constexpr std::size_t chunkSize = 1024 * 1024;

    const auto regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto regionSize = static_cast<std::size_t>(mbi.RegionSize);
    if (regionSize < patternLen) return 0;

    std::vector<std::uint8_t> buffer(chunkSize + overlapLen);
    const auto bufferAddress = reinterpret_cast<std::uintptr_t>(buffer.data());
    if (bufferAddress >= regionBase && bufferAddress < regionBase + regionSize) {
        return 0;
    }

    std::uint8_t overlap[overlapLen]{};
    std::size_t overlapSize = 0;
    int patched = 0;

    for (std::size_t offset = 0; offset < regionSize; offset += chunkSize) {
        const std::size_t bytesToRead = (std::min)(chunkSize, regionSize - offset);
        memcpy(buffer.data(), overlap, overlapSize);

        SIZE_T bytesRead = 0;
        const auto readAddress = reinterpret_cast<void*>(regionBase + offset);
        if (!ReadProcessMemory(GetCurrentProcess(), readAddress, buffer.data() + overlapSize, bytesToRead, &bytesRead) || bytesRead == 0) {
            ++stats.readFailures;
            overlapSize = 0;
            continue;
        }

        const std::size_t scanSize = overlapSize + static_cast<std::size_t>(bytesRead);
        const std::uintptr_t scanBase = regionBase + offset - overlapSize;

        if (scanSize >= patternLen) {
            for (std::size_t i = 0; i <= scanSize - patternLen; ++i) {
                if (!bytesAreTakefistsOnePattern(buffer.data() + i)) continue;

                auto* patchAddress = reinterpret_cast<std::uint8_t*>(scanBase + i);
                const auto patchValue = reinterpret_cast<std::uintptr_t>(patchAddress);
                if (patchValue < regionBase || patchValue + patternLen > regionBase + regionSize) {
                    continue;
                }

                if (tryPatchTakefistsPatternAt(patchAddress)) {
                    ++patched;
                    log("Takefists memory patch: OP_GetByte 1 -> 0 at %p (regionBase=%p regionSize=0x%llX)",
                        patchAddress,
                        mbi.BaseAddress,
                        static_cast<unsigned long long>(mbi.RegionSize));
                }
            }
        }

        overlapSize = (std::min)(overlapLen, scanSize);
        if (overlapSize > 0) {
            memcpy(overlap, buffer.data() + scanSize - overlapSize, overlapSize);
        }
    }

    SecureZeroMemory(buffer.data(), buffer.size());
    stats.patched += patched;
    return patched;
}

int scanAndPatchTakefistsOnce(int pass) {
    static constexpr std::size_t maxRegionSize = 64ull * 1024ull * 1024ull;

    SYSTEM_INFO info{};
    GetSystemInfo(&info);

    TakefistsScanStats stats{};
    auto address = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
    const auto maxAddress = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);

    while (address < maxAddress) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) {
            address += 0x10000;
            continue;
        }

        const auto regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto regionEnd = regionBase + static_cast<std::uintptr_t>(mbi.RegionSize);
        if (regionEnd <= address) break;

        const bool scanRegion = mbi.State == MEM_COMMIT &&
                                mbi.Type != MEM_IMAGE &&
                                isReadableMemoryProtection(mbi.Protect);

        if (scanRegion) {
            if (mbi.RegionSize <= maxRegionSize) {
                ++stats.scannedRegions;
                stats.scannedBytes += static_cast<std::uint64_t>(mbi.RegionSize);
                scanTakefistsRegion(mbi, stats);
            } else {
                ++stats.skippedLargeRegions;
            }
        }

        address = regionEnd;
    }

    log("Takefists memory patch pass %d: patched=%d scannedRegions=%llu scannedMB=%llu skippedLargeRegions=%llu readFailures=%llu",
        pass,
        stats.patched,
        static_cast<unsigned long long>(stats.scannedRegions),
        static_cast<unsigned long long>(stats.scannedBytes / (1024 * 1024)),
        static_cast<unsigned long long>(stats.skippedLargeRegions),
        static_cast<unsigned long long>(stats.readFailures));

    return stats.patched;
}

DWORD WINAPI takefistsPatchThread(LPVOID) {
    log("Takefists memory patch thread started; watching for stock GSC bytecode pattern 16 01 38 2A C6 00 00");

    int totalPatched = 0;
    for (int pass = 1; pass <= 24; ++pass) {
        Sleep(pass == 1 ? 8000 : 5000);
        totalPatched += scanAndPatchTakefistsOnce(pass);

        if (totalPatched >= 2) {
            log("Takefists memory patch target reached: totalPatched=%d", totalPatched);
            break;
        }
    }

    if (totalPatched == 0) {
        log("Takefists memory patch thread finished without finding the stock takefists bytecode pattern");
    }

    return 0;
}

bool isGunsmithVariantTableName(const std::string& rawName) {
    const std::string name = toLower(normalizeAssetName(rawName));
    return startsWith(name, "mp/gunsmith/") && endsWith(name, "_variants.csv");
}

bool isGunsmithVariantTableName(const char* rawName) {
    return rawName && *rawName && isGunsmithVariantTableName(std::string(rawName));
}

std::optional<std::string> gunsmithVariantRootFromAssetName(const std::string& assetName) {
    const std::string name = toLower(normalizeAssetName(assetName));
    const std::string prefix = "mp/gunsmith/";
    const std::string suffix = "_variants.csv";

    if (!startsWith(name, prefix) || !endsWith(name, suffix)) return std::nullopt;

    std::string stem = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    if (stem.empty()) return std::nullopt;
    if (!startsWith(stem, "iw8_")) stem = "iw8_" + stem;
    return stem;
}

bool isTargetStringTable(const char* rawName) {
    if (!rawName || !*rawName) return false;
    const std::string name = toLower(normalizeAssetName(rawName));
    return name == "mp/classtable_arena.csv" ||
           name == "mp/classtable_arena_alt.csv" ||
           name == "mp/classtable_arena_blueprints.csv" ||
           name == "mp/arenaggweapons.csv" ||
           name == "mp/arenaggweapons_alt.csv" ||
           isGunsmithVariantTableName(name);
}

bool isScriptProbeAssetName(const char* rawName) {
    if (!rawName || !*rawName) return false;

    const std::string name = toLower(normalizeAssetName(rawName));
    return name.find(".gsc") != std::string::npos ||
           name.find("scripts/mp/validation") != std::string::npos ||
           name.find("scripts/mp/gametypes/arena") != std::string::npos ||
           name == "validation" ||
           name == "arena" ||
           name == "arena_alt" ||
           name.find("/arena") != std::string::npos ||
           name.find("validation") != std::string::npos;
}

void logScriptProbeAsset(int type, const char* name, int allowCreateDefault, XAssetHeader result) {
    if (type == ASSET_TYPE_STRINGTABLE) return;
    if (!name || !isScriptProbeAssetName(name)) return;

    const std::string assetName = normalizeAssetName(name);
    const std::string key = std::to_string(type) + ":" + toLower(assetName);

    if (g_loggedScriptProbeByAsset[key]) return;
    g_loggedScriptProbeByAsset[key] = true;

    log("Script asset probe: type=0x%X (%d) name='%s' allowCreateDefault=%d result=%p",
        type,
        type,
        assetName.c_str(),
        allowCreateDefault,
        result.data);
}

std::uint32_t jenkinsOneAtATime(const std::string& s) {
    std::uint32_t hash = 0;
    for (unsigned char c : s) {
        hash += c;
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

std::optional<CsvTable> parseCsv(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;

    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() >= 3 &&
        static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data.erase(0, 3);
    }

    CsvTable out{};
    std::vector<std::string> row{};
    std::string cell{};
    bool quoted = false;

    auto finishCell = [&]() {
        row.emplace_back(std::move(cell));
        cell.clear();
    };

    auto finishRow = [&]() {
        finishCell();
        if (!(row.size() == 1 && row[0].empty())) {
            out.cols = (std::max)(out.cols, row.size());
            out.rows.emplace_back(std::move(row));
        }
        row.clear();
    };

    for (std::size_t i = 0; i < data.size(); ++i) {
        const char ch = data[i];
        if (quoted) {
            if (ch == '"') {
                if (i + 1 < data.size() && data[i + 1] == '"') {
                    cell.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cell.push_back(ch);
            }
        } else {
            if (ch == '"') {
                quoted = true;
            } else if (ch == ',') {
                finishCell();
            } else if (ch == '\n') {
                finishRow();
            } else if (ch == '\r') {
                // Ignore CR in CRLF.
            } else {
                cell.push_back(ch);
            }
        }
    }

    if (quoted) return std::nullopt;
    if (!cell.empty() || !row.empty()) finishRow();
    if (out.rows.empty() || out.cols == 0) return std::nullopt;

    for (auto& r : out.rows) r.resize(out.cols);
    return out;
}

std::optional<std::size_t> findCsvRowByLabel(const CsvTable& csv, const std::string& label) {
    const std::string wanted = toLower(label);
    for (std::size_t i = 0; i < csv.rows.size(); ++i) {
        if (!csv.rows[i].empty() && toLower(trim(csv.rows[i][0])) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

std::string csvCellTrimmed(const CsvTable& csv, std::size_t row, std::size_t col) {
    if (row >= csv.rows.size() || col >= csv.rows[row].size()) return {};
    return trim(csv.rows[row][col]);
}

std::vector<std::string> splitListTokens(const std::string& raw) {
    std::vector<std::string> out{};
    std::string current{};

    auto flush = [&]() {
        std::string value = trim(current);
        if (!value.empty()) out.emplace_back(std::move(value));
        current.clear();
    };

    for (char ch : raw) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isspace(uch) || ch == ',' || ch == ';') {
            flush();
        } else {
            current.push_back(ch);
        }
    }

    flush();
    return out;
}

std::string makeCustomAttachmentCell(const std::string& attachmentToken) {
    if (attachmentToken.find('|') != std::string::npos) return attachmentToken;
    return attachmentToken + "|0";
}

std::optional<std::size_t> findAttachmentRow(const CsvTable& csv, const std::string& prefix, int index) {
    auto numbered = findCsvRowByLabel(csv, prefix + std::to_string(index));
    if (numbered || index != 1) return numbered;
    return findCsvRowByLabel(csv, prefix);
}

void collectBlueprintAttachmentPatches(
    const CsvTable& csv,
    const std::string& rootLabel,
    const std::string& variantLabel,
    const std::string& attachmentPrefix,
    BlueprintAttachmentMap& out,
    int* patchCount
) {
    auto rootRow = findCsvRowByLabel(csv, rootLabel);
    if (!rootRow) return;

    const auto variantRow = findCsvRowByLabel(csv, variantLabel);
    std::vector<std::optional<std::size_t>> attachmentRows{};
    attachmentRows.reserve(5);
    for (int i = 1; i <= 5; ++i) {
        attachmentRows.emplace_back(findAttachmentRow(csv, attachmentPrefix, i));
    }

    for (std::size_t col = 1; col < csv.cols; ++col) {
        std::string root = csvCellTrimmed(csv, *rootRow, col);
        if (isEmptyOrNoneValue(root)) continue;

        std::vector<std::string> attachments{};
        attachments.reserve(5);
        for (const auto& row : attachmentRows) {
            if (!row) continue;
            std::string value = csvCellTrimmed(csv, *row, col);
            if (isEmptyOrNoneValue(value)) continue;
            attachments.emplace_back(std::move(value));
        }
        if (attachments.empty()) continue;

        std::vector<std::string> variantIds{};
        bool variantCellHadValue = false;
        if (variantRow) {
            const std::string rawVariantIds = csvCellTrimmed(csv, *variantRow, col);
            variantCellHadValue = !isEmptyOrNoneValue(rawVariantIds);
            for (std::string id : splitListTokens(rawVariantIds)) {
                const std::string lowerId = toLower(id);
                if (lowerId == "none" || lowerId == "null" || lowerId == "-1") continue;
                variantIds.emplace_back(std::move(id));
            }
        }
        if (variantCellHadValue && variantIds.empty()) continue;

        BlueprintAttachmentPatch patch{};
        patch.variantIds = std::move(variantIds);
        patch.attachments = std::move(attachments);
        patch.sourceColumn = static_cast<int>(col);
        out[toLower(root)].emplace_back(std::move(patch));
        if (patchCount) ++(*patchCount);
    }
}

BlueprintAttachmentMap buildBlueprintAttachmentMap(const CsvTable& csv, int* patchCount = nullptr) {
    if (patchCount) *patchCount = 0;
    BlueprintAttachmentMap out{};
    collectBlueprintAttachmentPatches(csv, "loadoutPrimary", "loadoutPrimaryVariantID", "loadoutPrimaryAttachment", out, patchCount);
    collectBlueprintAttachmentPatches(csv, "loadoutSecondary", "loadoutSecondaryVariantID", "loadoutSecondaryAttachment", out, patchCount);
    return out;
}

const BlueprintAttachmentPatch* selectBlueprintPatchForVariant(
    const std::vector<BlueprintAttachmentPatch>& patches,
    const std::string& variantId
) {
    const BlueprintAttachmentPatch* fallback = nullptr;
    for (const auto& patch : patches) {
        if (patch.variantIds.empty()) {
            if (!fallback) fallback = &patch;
            continue;
        }

        if (std::find(patch.variantIds.begin(), patch.variantIds.end(), variantId) != patch.variantIds.end()) {
            return &patch;
        }
    }
    return fallback;
}

std::optional<fs::path> findLooseAssetFile(const std::string& assetName) {
    const auto normalized = normalizeAssetName(assetName);
    const auto lower = toLower(normalized);

    std::vector<std::string> candidates{};
    candidates.emplace_back(normalized);
    candidates.emplace_back(lower);

    // Useful fallback for users who still name the file classTable_*.csv.
    if (lower == "mp/classtable_arena.csv") {
        candidates.emplace_back("mp/classTable_arena.csv");
    } else if (lower == "mp/classtable_arena_alt.csv") {
        candidates.emplace_back("mp/classTable_arena_alt.csv");
    } else if (lower == "mp/classtable_arena_blueprints.csv") {
        candidates.emplace_back("mp/classTable_arena_blueprints.csv");
    } else if (lower == "mp/arenaggweapons.csv") {
        candidates.emplace_back("mp/arenaGGWeapons.csv");
    } else if (lower == "mp/arenaggweapons_alt.csv") {
        candidates.emplace_back("mp/arenaGGWeapons_alt.csv");
    }

    for (const auto& root : g_assetRoots) {
        for (const auto& rel : candidates) {
            const auto path = root / fs::path(rel);
            if (fs::exists(path)) return path;
        }
    }

    return std::nullopt;
}

__declspec(noinline) bool safePatchStringTable(
    StringTable* target,
    const char* name,
    int columns,
    int rows,
    int unique,
    std::uint16_t* cellIndices,
    int* hashes,
    const char** strings
) {
    __try {
        if (!target) return false;

        DWORD oldProtect = 0;
        if (!VirtualProtect(target, sizeof(StringTable), PAGE_READWRITE, &oldProtect)) {
            return false;
        }

        target->name = name;
        target->columnCount = columns;
        target->rowCount = rows;
        target->uniqueCellCount = unique;
        target->cellIndices = cellIndices;
        target->hashes = hashes;
        target->strings = strings;

        DWORD ignored = 0;
        VirtualProtect(target, sizeof(StringTable), oldProtect, &ignored);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


std::string makeActiveKey(const std::string& assetName, StringTable* table) {
    char ptr[64]{};
    snprintf(ptr, sizeof(ptr), "%p", static_cast<void*>(table));
    return toLower(normalizeAssetName(assetName)) + "@" + ptr;
}

const char* tableCellString(StringTable* table, int row, int col) {
    if (!table || row < 0 || col < 0 || row >= table->rowCount || col >= table->columnCount) return "";
    if (!table->cellIndices || !table->strings || table->uniqueCellCount <= 0) return "";

    const auto flat = static_cast<std::size_t>(row) * static_cast<std::size_t>(table->columnCount) + static_cast<std::size_t>(col);
    const auto idx = table->cellIndices[flat];
    if (idx >= table->uniqueCellCount) return "";
    return table->strings[idx] ? table->strings[idx] : "";
}

std::string defaultForMissingRow(const std::string& label, int dstCol) {
    if (label == "loadoutName") return "custom_loop_" + std::to_string(dstCol);
    if (label == "loadoutArchetype") return "archetype_assault";
    if (label == "loadoutPrimaryAddBlueprintAttachments") return "0";
    if (label == "loadoutSecondaryAddBlueprintAttachments") return "0";
    if (label == "loadoutPrimaryVariantID") return "0";
    if (label == "loadoutSecondaryVariantID") return "-1";
    if (label == "loadoutOverkill") return "0";
    if (label == "loadoutPerk1") return "specialty_quickfix";
    if (label == "loadoutPerk2") return "specialty_hardline";
    if (label == "loadoutPerk3") return "specialty_amp";
    return "none";
}


std::optional<fs::path> findExtraPoolFile() {
    const std::vector<std::string> candidates{
        "mp/wz_gulag_extra_string_pool_for_sidecar.csv",
        "wz_gulag_extra_string_pool_for_sidecar.csv",
        "mp/extra_string_pool.csv",
        "extra_string_pool.csv",
        "mp/expanded_string_pool.csv",
        "expanded_string_pool.csv"
    };

    for (const auto& root : g_assetRoots) {
        for (const auto& rel : candidates) {
            const auto path = root / fs::path(rel);
            if (fs::exists(path)) return path;
        }
    }

    return std::nullopt;
}

std::vector<std::string> loadExtraPoolStrings(fs::path* foundPath = nullptr) {
    if (foundPath) *foundPath = fs::path{};
    std::vector<std::string> values{};

    auto path = findExtraPoolFile();
    if (!path) return values;
    if (foundPath) *foundPath = *path;

    auto csv = parseCsv(*path);
    if (!csv) {
        log("Failed to parse external string pool CSV '%s'", path->string().c_str());
        return values;
    }

    for (std::size_t r = 0; r < csv->rows.size(); ++r) {
        for (std::size_t c = 0; c < csv->rows[r].size(); ++c) {
            std::string v = csv->rows[r][c];
            if (v.empty()) continue;
            if (r == 0 && c == 0 && toLower(v) == "string") continue;
            values.emplace_back(std::move(v));
        }
    }

    return values;
}


bool isGenericOspWeaponTable(const std::string& assetName) {
    const std::string lower = toLower(normalizeAssetName(assetName));
    return lower == "mp/arenaggweapons.csv" || lower == "mp/arenaggweapons_alt.csv";
}

bool replaceGenericStringTable(StringTable* table, const std::string& assetName, const CsvTable& csv, const fs::file_time_type& lastWrite) {
    if (!table) return false;

    const int targetRows = table->rowCount;
    const int targetCols = table->columnCount;
    const int originalUnique = table->uniqueCellCount;
    const char* originalName = table->name;
    int* originalHashes = table->hashes;
    const char** originalStrings = table->strings;
    std::uint16_t* originalCellIndices = table->cellIndices;

    if (targetRows <= 0 || targetCols <= 0) {
        log("Refusing loose generic string_table '%s': target table has invalid shape (%d rows, %d cols)", assetName.c_str(), targetRows, targetCols);
        return false;
    }
    if (!originalCellIndices || !originalStrings || originalUnique <= 0) {
        log("Refusing loose generic string_table '%s': target table has invalid backing arrays (unique=%d)", assetName.c_str(), originalUnique);
        return false;
    }

    const int sourceRows = static_cast<int>(csv.rows.size());
    const int sourceCols = static_cast<int>(csv.cols);
    if (sourceRows <= 0 || sourceCols <= 0) {
        log("Refusing loose generic string_table '%s': source CSV has invalid shape (%d rows, %d cols)", assetName.c_str(), sourceRows, sourceCols);
        return false;
    }

    int sourceStartRow = 0;
    if (!csv.rows.empty() && !csv.rows[0].empty()) {
        const std::string first = toLower(csv.rows[0][0]);
        if (first == "weaponname" || first == "<column 0>") {
            sourceStartRow = 1;
        }
    }
    const int usableRows = sourceRows - sourceStartRow;
    if (usableRows <= 0) {
        log("Refusing loose generic string_table '%s': source CSV has only a header row", assetName.c_str());
        return false;
    }

    const auto cellCount = static_cast<std::size_t>(targetRows) * static_cast<std::size_t>(targetCols);

    fs::path extraPoolPath{};
    std::vector<std::string> extraPoolStrings = loadExtraPoolStrings(&extraPoolPath);

    std::unordered_map<std::string, std::uint16_t> expandedIndexByString{};
    expandedIndexByString.reserve(static_cast<std::size_t>(originalUnique) + extraPoolStrings.size());

    auto originalCell = [&](int row, int col, std::uint16_t* outIndex = nullptr) -> std::string {
        if (outIndex) *outIndex = 0;
        if (row < 0 || col < 0 || row >= targetRows || col >= targetCols) return {};
        const auto flat = static_cast<std::size_t>(row) * static_cast<std::size_t>(targetCols) + static_cast<std::size_t>(col);
        const std::uint16_t idx = originalCellIndices[flat];
        if (outIndex) *outIndex = idx;
        if (idx >= originalUnique) return {};
        return originalStrings[idx] ? std::string(originalStrings[idx]) : std::string{};
    };

    auto owned = std::make_unique<OwnedTable>();
    owned->assetName = normalizeAssetName(assetName);
    owned->lastWrite = lastWrite;
    owned->target = table;
    owned->cellIndices.resize(cellCount);

    owned->stringPointers.reserve(static_cast<std::size_t>(originalUnique) + extraPoolStrings.size());
    owned->hashes.reserve(static_cast<std::size_t>(originalUnique) + extraPoolStrings.size());
    for (int i = 0; i < originalUnique; ++i) {
        const char* s = originalStrings[i] ? originalStrings[i] : "";
        owned->stringPointers.push_back(s);
        owned->hashes.push_back(originalHashes ? originalHashes[i] : static_cast<int>(jenkinsOneAtATime(std::string(s))));
        expandedIndexByString.emplace(std::string(s), static_cast<std::uint16_t>(i));
    }

    int appendedExtraStrings = 0;
    int skippedDuplicateExtraStrings = 0;
    int skippedOverflowExtraStrings = 0;
    owned->stringStorage.reserve(extraPoolStrings.size());
    for (const auto& v : extraPoolStrings) {
        if (v.empty()) continue;
        if (expandedIndexByString.find(v) != expandedIndexByString.end()) {
            ++skippedDuplicateExtraStrings;
            continue;
        }
        const std::size_t nextIndex = owned->stringPointers.size();
        if (nextIndex > 0xFFFFu) {
            ++skippedOverflowExtraStrings;
            continue;
        }
        owned->stringStorage.emplace_back(v);
        owned->stringPointers.push_back(owned->stringStorage.back().c_str());
        owned->hashes.push_back(static_cast<int>(jenkinsOneAtATime(v)));
        expandedIndexByString.emplace(v, static_cast<std::uint16_t>(nextIndex));
        ++appendedExtraStrings;
    }

    const int expandedUnique = static_cast<int>(owned->stringPointers.size());

    int changedCells = 0, unchangedCells = 0, missingStrings = 0, extraPoolCells = 0, loopedRows = 0;
    for (int dstRow = 0; dstRow < targetRows; ++dstRow) {
        const int srcRow = sourceStartRow + (dstRow % usableRows);
        if (dstRow >= usableRows) ++loopedRows;

        for (int dstCol = 0; dstCol < targetCols; ++dstCol) {
            const auto flat = static_cast<std::size_t>(dstRow) * static_cast<std::size_t>(targetCols) + static_cast<std::size_t>(dstCol);
            std::uint16_t originalIndex = 0;
            const std::string originalValue = originalCell(dstRow, dstCol, &originalIndex);
            std::string desiredValue = originalValue;

            if (srcRow >= 0 && srcRow < static_cast<int>(csv.rows.size()) && dstCol < static_cast<int>(csv.rows[srcRow].size())) {
                desiredValue = csv.rows[srcRow][dstCol];
            }

            if (desiredValue == originalValue) {
                owned->cellIndices[flat] = originalIndex;
                ++unchangedCells;
                continue;
            }

            auto it = expandedIndexByString.find(desiredValue);
            if (it != expandedIndexByString.end()) {
                owned->cellIndices[flat] = it->second;
                ++changedCells;
                if (it->second >= originalUnique) ++extraPoolCells;
            } else {
                owned->cellIndices[flat] = originalIndex;
                ++missingStrings;
            }
        }
    }

    if (!safePatchStringTable(table, originalName, targetCols, targetRows, expandedUnique,
                              owned->cellIndices.data(), owned->hashes.data(), owned->stringPointers.data())) {
        log("Failed to overlay generic string_table cell-index memory for '%s'", assetName.c_str());
        return false;
    }

    const auto activeKey = makeActiveKey(assetName, table);
    OwnedTable* active = owned.get();
    g_ownedPool.emplace_back(std::move(owned));
    g_active[activeKey] = active;

    if (loopedRows > 0) {
        log("Generic OSP table safety net: source CSV has %d usable data rows; loop-filled %d target rows", usableRows, loopedRows);
    }
    if (!extraPoolPath.empty()) {
        log("External pool loaded for generic '%s': path='%s' sourceValues=%llu appendedExtraStrings=%d skippedDuplicateExtraStrings=%d skippedOverflowExtraStrings=%d",
            assetName.c_str(), extraPoolPath.string().c_str(),
            static_cast<unsigned long long>(extraPoolStrings.size()), appendedExtraStrings,
            skippedDuplicateExtraStrings, skippedOverflowExtraStrings);
    }
    log("Generic OSP table overlay stats for '%s': changedCells=%d unchangedCells=%d extraPoolCells=%d missingStringsKeptStock=%d originalUnique=%d expandedUnique=%d cellCount=%llu",
        assetName.c_str(), changedCells, unchangedCells, extraPoolCells, missingStrings, originalUnique, expandedUnique,
        static_cast<unsigned long long>(cellCount));

    return true;
}

bool replaceGunsmithVariantTable(StringTable* table, const std::string& assetName, const CsvTable& blueprintCsv, const fs::file_time_type& lastWrite) {
    if (!table) return false;

    const int targetRows = table->rowCount;
    const int targetCols = table->columnCount;
    const int originalUnique = table->uniqueCellCount;
    const char* originalName = table->name;
    int* originalHashes = table->hashes;
    const char** originalStrings = table->strings;
    std::uint16_t* originalCellIndices = table->cellIndices;

    if (targetRows <= 0 || targetCols <= 15) {
        log("Refusing gunsmith variant attachment overlay '%s': target table has invalid shape (%d rows, %d cols)",
            assetName.c_str(), targetRows, targetCols);
        return false;
    }
    if (!originalCellIndices || !originalStrings || originalUnique <= 0) {
        log("Refusing gunsmith variant attachment overlay '%s': target table has invalid backing arrays (unique=%d)",
            assetName.c_str(), originalUnique);
        return false;
    }

    auto root = gunsmithVariantRootFromAssetName(assetName);
    if (!root) return false;

    int blueprintPatchCount = 0;
    BlueprintAttachmentMap patchesByRoot = buildBlueprintAttachmentMap(blueprintCsv, &blueprintPatchCount);
    auto patchIt = patchesByRoot.find(*root);
    if (patchIt == patchesByRoot.end() || patchIt->second.empty()) {
        return false;
    }

    const std::vector<BlueprintAttachmentPatch>& patches = patchIt->second;
    const auto cellCount = static_cast<std::size_t>(targetRows) * static_cast<std::size_t>(targetCols);
    if (cellCount == 0) {
        log("Refusing gunsmith variant attachment overlay '%s': target cell count is zero", assetName.c_str());
        return false;
    }

    std::size_t maxGeneratedStrings = 1;
    for (const auto& patch : patches) {
        maxGeneratedStrings += patch.attachments.size();
    }

    std::unordered_map<std::string, std::uint16_t> expandedIndexByString{};
    expandedIndexByString.reserve(static_cast<std::size_t>(originalUnique) + maxGeneratedStrings);

    auto owned = std::make_unique<OwnedTable>();
    owned->assetName = normalizeAssetName(assetName);
    owned->lastWrite = lastWrite;
    owned->target = table;
    owned->cellIndices.assign(originalCellIndices, originalCellIndices + cellCount);
    owned->stringStorage.reserve(maxGeneratedStrings);
    owned->stringPointers.reserve(static_cast<std::size_t>(originalUnique) + maxGeneratedStrings);
    owned->hashes.reserve(static_cast<std::size_t>(originalUnique) + maxGeneratedStrings);

    for (int i = 0; i < originalUnique; ++i) {
        const char* s = originalStrings[i] ? originalStrings[i] : "";
        owned->stringPointers.push_back(s);
        owned->hashes.push_back(originalHashes ? originalHashes[i] : static_cast<int>(jenkinsOneAtATime(std::string(s))));
        expandedIndexByString.emplace(std::string(s), static_cast<std::uint16_t>(i));
    }

    int appendedGeneratedStrings = 0;
    auto ensureStringIndex = [&](const std::string& value, std::uint16_t& outIndex) -> bool {
        auto it = expandedIndexByString.find(value);
        if (it != expandedIndexByString.end()) {
            outIndex = it->second;
            return true;
        }

        const std::size_t nextIndex = owned->stringPointers.size();
        if (nextIndex > 0xFFFFu) {
            return false;
        }

        owned->stringStorage.emplace_back(value);
        owned->stringPointers.push_back(owned->stringStorage.back().c_str());
        owned->hashes.push_back(static_cast<int>(jenkinsOneAtATime(value)));
        outIndex = static_cast<std::uint16_t>(nextIndex);
        expandedIndexByString.emplace(value, outIndex);
        ++appendedGeneratedStrings;
        return true;
    };

    auto originalCell = [&](int row, int col, std::uint16_t* outIndex = nullptr) -> std::string {
        if (outIndex) *outIndex = 0;
        if (row < 0 || col < 0 || row >= targetRows || col >= targetCols) return {};
        const auto flat = static_cast<std::size_t>(row) * static_cast<std::size_t>(targetCols) + static_cast<std::size_t>(col);
        const std::uint16_t idx = originalCellIndices[flat];
        if (outIndex) *outIndex = idx;
        if (idx >= originalUnique) return {};
        return originalStrings[idx] ? std::string(originalStrings[idx]) : std::string{};
    };

    int rootRows = 0;
    int patchedRows = 0;
    int unmatchedVariantRows = 0;
    int changedCustomCells = 0;
    int unchangedCustomCells = 0;
    int clearedCustomCells = 0;
    int generatedAttachmentCells = 0;
    int truncatedAttachments = 0;

    const int customStartCol = 5;
    const int customEndCol = (std::min)(15, targetCols - 1);
    const int customColCount = customEndCol - customStartCol + 1;

    for (int row = 0; row < targetRows; ++row) {
        const std::string rowRoot = toLower(trim(originalCell(row, 2)));
        if (rowRoot != *root) continue;
        ++rootRows;

        const std::string rowVariantId = trim(originalCell(row, 0));
        const BlueprintAttachmentPatch* patch = selectBlueprintPatchForVariant(patches, rowVariantId);
        if (!patch) {
            ++unmatchedVariantRows;
            continue;
        }

        ++patchedRows;
        if (static_cast<int>(patch->attachments.size()) > customColCount) {
            truncatedAttachments += static_cast<int>(patch->attachments.size()) - customColCount;
        }

        for (int slot = 0; slot < customColCount; ++slot) {
            std::string desiredValue{};
            if (slot < static_cast<int>(patch->attachments.size())) {
                desiredValue = makeCustomAttachmentCell(patch->attachments[slot]);
                ++generatedAttachmentCells;
            }

            std::uint16_t desiredIndex = 0;
            if (!ensureStringIndex(desiredValue, desiredIndex)) {
                log("Refusing gunsmith variant attachment overlay '%s': generated string pool would exceed uint16 cell-index limit",
                    assetName.c_str());
                return false;
            }

            const int col = customStartCol + slot;
            const auto flat = static_cast<std::size_t>(row) * static_cast<std::size_t>(targetCols) + static_cast<std::size_t>(col);
            const std::string originalValue = originalCell(row, col);
            owned->cellIndices[flat] = desiredIndex;

            if (desiredValue == originalValue) {
                ++unchangedCustomCells;
            } else {
                ++changedCustomCells;
                if (desiredValue.empty()) ++clearedCustomCells;
            }
        }
    }

    if (patchedRows <= 0) {
        log("Blueprint gunsmith attachment source found root '%s' for '%s', but no target variant rows matched (rootRows=%d unmatchedVariantRows=%d blueprintPatchColumns=%d)",
            root->c_str(), assetName.c_str(), rootRows, unmatchedVariantRows, blueprintPatchCount);
        return false;
    }

    const int expandedUnique = static_cast<int>(owned->stringPointers.size());
    if (!safePatchStringTable(
        table,
        originalName,
        targetCols,
        targetRows,
        expandedUnique,
        owned->cellIndices.data(),
        owned->hashes.data(),
        owned->stringPointers.data())) {
        log("Failed to overlay gunsmith variant attachment cell-index memory for '%s'", assetName.c_str());
        return false;
    }

    const auto activeKey = makeActiveKey(assetName, table);
    OwnedTable* active = owned.get();
    g_ownedPool.emplace_back(std::move(owned));
    g_active[activeKey] = active;

    if (truncatedAttachments > 0) {
        log("Gunsmith variant attachment overlay truncated %d attachment cells for '%s' because the target table exposes only columns %d-%d",
            truncatedAttachments, assetName.c_str(), customStartCol, customEndCol);
    }
    log("Gunsmith variant attachment overlay stats for '%s': root='%s' blueprintPatchColumns=%d rootRows=%d patchedRows=%d unmatchedVariantRows=%d generatedAttachmentCells=%d changedCustomCells=%d unchangedCustomCells=%d clearedCustomCells=%d appendedGeneratedStrings=%d originalUnique=%d expandedUnique=%d",
        assetName.c_str(), root->c_str(), blueprintPatchCount, rootRows, patchedRows, unmatchedVariantRows,
        generatedAttachmentCells, changedCustomCells, unchangedCustomCells, clearedCustomCells,
        appendedGeneratedStrings, originalUnique, expandedUnique);

    return true;
}


bool replaceStringTable(StringTable* table, const std::string& assetName, const CsvTable& csv, const fs::file_time_type& lastWrite) {
    if (!table) return false;

    const int targetRows = table->rowCount;
    const int targetCols = table->columnCount;
    const int originalUnique = table->uniqueCellCount;
    const char* originalName = table->name;
    int* originalHashes = table->hashes;
    const char** originalStrings = table->strings;
    std::uint16_t* originalCellIndices = table->cellIndices;

    if (targetRows <= 0 || targetCols <= 1) {
        log("Refusing loose string_table '%s': target table has invalid shape (%d rows, %d cols)", assetName.c_str(), targetRows, targetCols);
        return false;
    }
    if (!originalCellIndices || !originalStrings || originalUnique <= 0) {
        log("Refusing loose string_table '%s': target table has invalid backing arrays (unique=%d)", assetName.c_str(), originalUnique);
        return false;
    }

    const int sourceRows = static_cast<int>(csv.rows.size());
    const int sourceCols = static_cast<int>(csv.cols);
    const int sourceClassCols = sourceCols - 1;
    if (sourceRows <= 0 || sourceClassCols <= 0) {
        log("Refusing loose string_table '%s': source CSV has no class columns (%d rows, %d cols)", assetName.c_str(), sourceRows, sourceCols);
        return false;
    }

    const auto cellCount = static_cast<std::size_t>(targetRows) * static_cast<std::size_t>(targetCols);
    if (cellCount == 0) {
        log("Refusing loose string_table '%s': target cell count is zero", assetName.c_str());
        return false;
    }

    // Build a source row lookup by first-column label. The CSV dump usually includes a generated
    // '<column 0>,<column 1>,...' header row, while the live IW8 StringTable does not. Matching
    // by row label avoids off-by-one row errors.
    std::unordered_map<std::string, std::size_t> sourceRowByLabel{};
    sourceRowByLabel.reserve(csv.rows.size());
    for (std::size_t i = 0; i < csv.rows.size(); ++i) {
        if (!csv.rows[i].empty() && !csv.rows[i][0].empty()) {
            sourceRowByLabel[csv.rows[i][0]] = i;
        }
    }

    if (!sourceRowByLabel.count("loadoutName") || !sourceRowByLabel.count("loadoutPrimary")) {
        log("Refusing loose string_table '%s': source CSV must include at least loadoutName and loadoutPrimary rows", assetName.c_str());
        return false;
    }

    // Stable base from stock-pool-index build:
    // Keep IW8's exact original unique-string/hash pool first, in its original order. That made
    // the stock no-op table load correctly. This build then appends optional external CSV strings
    // after the stock pool, so original Arena row labels/hash ordering remain untouched.
    fs::path extraPoolPath{};
    std::vector<std::string> extraPoolStrings = loadExtraPoolStrings(&extraPoolPath);

    std::unordered_map<std::string, std::uint16_t> expandedIndexByString{};
    expandedIndexByString.reserve(static_cast<std::size_t>(originalUnique) + extraPoolStrings.size());

    auto originalCell = [&](int row, int col, std::uint16_t* outIndex = nullptr) -> std::string {
        if (outIndex) *outIndex = 0;
        if (row < 0 || col < 0 || row >= targetRows || col >= targetCols) return {};
        const auto flat = static_cast<std::size_t>(row) * static_cast<std::size_t>(targetCols) + static_cast<std::size_t>(col);
        const std::uint16_t idx = originalCellIndices[flat];
        if (outIndex) *outIndex = idx;
        if (idx >= originalUnique) return {};
        return originalStrings[idx] ? std::string(originalStrings[idx]) : std::string{};
    };

    auto fallbackString = [&](const std::string& label, int dstCol) -> std::string {
        if (label == "loadoutPrimaryAddBlueprintAttachments") return "0";
        if (label == "loadoutSecondaryAddBlueprintAttachments") return "0";
        if (label == "loadoutPrimaryVariantID") return "0";
        if (label == "loadoutSecondaryVariantID") return "-1";
        if (label == "loadoutOverkill") return "0";
        if (label == "loadoutPerk1") return "specialty_quickfix";
        if (label == "loadoutPerk2") return "specialty_hardline";
        if (label == "loadoutPerk3") return "specialty_amp";
        return "none";
    };

    auto owned = std::make_unique<OwnedTable>();
    owned->assetName = normalizeAssetName(assetName);
    owned->lastWrite = lastWrite;
    owned->target = table;
    owned->cellIndices.resize(cellCount);

    // Preserve all original pool entries at their exact original indices.
    owned->stringPointers.reserve(static_cast<std::size_t>(originalUnique) + extraPoolStrings.size());
    owned->hashes.reserve(static_cast<std::size_t>(originalUnique) + extraPoolStrings.size());
    for (int i = 0; i < originalUnique; ++i) {
        const char* s = originalStrings[i] ? originalStrings[i] : "";
        owned->stringPointers.push_back(s);
        owned->hashes.push_back(originalHashes ? originalHashes[i] : static_cast<int>(jenkinsOneAtATime(std::string(s))));
        expandedIndexByString.emplace(std::string(s), static_cast<std::uint16_t>(i));
    }

    int appendedExtraStrings = 0;
    int skippedDuplicateExtraStrings = 0;
    int skippedOverflowExtraStrings = 0;
    owned->stringStorage.reserve(extraPoolStrings.size());
    for (const auto& v : extraPoolStrings) {
        if (v.empty()) continue;
        if (expandedIndexByString.find(v) != expandedIndexByString.end()) {
            ++skippedDuplicateExtraStrings;
            continue;
        }
        const std::size_t nextIndex = owned->stringPointers.size();
        if (nextIndex > 0xFFFFu) {
            ++skippedOverflowExtraStrings;
            continue;
        }
        owned->stringStorage.emplace_back(v);
        owned->stringPointers.push_back(owned->stringStorage.back().c_str());
        owned->hashes.push_back(static_cast<int>(jenkinsOneAtATime(v)));
        expandedIndexByString.emplace(v, static_cast<std::uint16_t>(nextIndex));
        ++appendedExtraStrings;
    }

    const int expandedUnique = static_cast<int>(owned->stringPointers.size());

    int changedCells = 0;
    int unchangedCells = 0;
    int missingRows = 0;
    int missingStrings = 0;
    int extraPoolCells = 0;
    int loopedColumns = 0;

    for (int dstRow = 0; dstRow < targetRows; ++dstRow) {
        std::uint16_t labelIndex = 0;
        std::string label = originalCell(dstRow, 0, &labelIndex);
        if (label.empty() && dstRow < sourceRows && !csv.rows[dstRow].empty()) {
            label = csv.rows[dstRow][0];
        }

        auto srcIt = sourceRowByLabel.find(label);
        const std::vector<std::string>* srcRow = nullptr;
        if (srcIt != sourceRowByLabel.end()) {
            srcRow = &csv.rows[srcIt->second];
        } else if (dstRow < sourceRows) {
            srcRow = &csv.rows[dstRow];
            ++missingRows;
        } else {
            ++missingRows;
        }

        for (int dstCol = 0; dstCol < targetCols; ++dstCol) {
            const auto flat = static_cast<std::size_t>(dstRow) * static_cast<std::size_t>(targetCols) + static_cast<std::size_t>(dstCol);
            std::uint16_t originalIndex = 0;
            const std::string originalValue = originalCell(dstRow, dstCol, &originalIndex);
            std::string desiredValue;

            if (dstCol == 0) {
                // Keep engine row labels exactly.
                desiredValue = originalValue;
            } else if (srcRow) {
                const int srcCol = 1 + ((dstCol - 1) % sourceClassCols);
                if (srcCol < static_cast<int>(srcRow->size())) {
                    desiredValue = (*srcRow)[srcCol];
                } else {
                    desiredValue = fallbackString(label, dstCol);
                }
                if (dstCol - 1 >= sourceClassCols) ++loopedColumns;
            } else {
                desiredValue = fallbackString(label, dstCol);
            }

            // Exact no-op cells keep their exact original index. This matters if IW8 has duplicate
            // equal strings in the unique pool for reasons we do not understand yet.
            if (desiredValue == originalValue) {
                owned->cellIndices[flat] = originalIndex;
                ++unchangedCells;
                continue;
            }

            auto it = expandedIndexByString.find(desiredValue);
            if (it != expandedIndexByString.end()) {
                owned->cellIndices[flat] = it->second;
                ++changedCells;
                if (it->second >= originalUnique) ++extraPoolCells;
            } else {
                // Still unknown even after external pool. Leave the original stock cell.
                owned->cellIndices[flat] = originalIndex;
                ++missingStrings;
            }
        }
    }

    if (!safePatchStringTable(
        table,
        originalName,
        targetCols,
        targetRows,
        expandedUnique,
        owned->cellIndices.data(),
        owned->hashes.data(),
        owned->stringPointers.data())) {
        log("Failed to overlay string_table cell-index memory for '%s'", assetName.c_str());
        return false;
    }

    const auto activeKey = makeActiveKey(assetName, table);
    OwnedTable* active = owned.get();
    g_ownedPool.emplace_back(std::move(owned));
    g_active[activeKey] = active;

    if (sourceClassCols < targetCols - 1) {
        log("Safety net: source CSV has %d class columns; loop-filled %d target class columns to cover %d total classes",
            sourceClassCols, loopedColumns, targetCols - 1);
    }
    if (missingRows > 0) {
        log("Warning: source CSV was missing %d target row labels; defaults/fallback rows were used", missingRows);
    }
    if (!extraPoolPath.empty()) {
        log("External pool loaded for '%s': path='%s' sourceValues=%llu appendedExtraStrings=%d skippedDuplicateExtraStrings=%d skippedOverflowExtraStrings=%d",
            assetName.c_str(), extraPoolPath.string().c_str(),
            static_cast<unsigned long long>(extraPoolStrings.size()), appendedExtraStrings,
            skippedDuplicateExtraStrings, skippedOverflowExtraStrings);
    } else {
        log("No external string pool CSV found; using Arena stock pool only for '%s'", assetName.c_str());
    }
    log("Expanded-pool index overlay stats for '%s': changedCells=%d unchangedCells=%d extraPoolCells=%d missingStringsKeptStock=%d originalUnique=%d expandedUnique=%d cellCount=%llu",
        assetName.c_str(), changedCells, unchangedCells, extraPoolCells, missingStrings, originalUnique, expandedUnique,
        static_cast<unsigned long long>(cellCount));

    return true;
}

StringTable* tryReplaceStringTable(const char* givenName, StringTable* table) {
    if (!givenName || !table) return table;
    if (!isTargetStringTable(givenName)) return table;

    std::lock_guard<std::mutex> lock(g_replaceMutex);

    const auto assetName = normalizeAssetName(givenName);
    const auto activeKey = makeActiveKey(assetName, table);
    const auto assetKey = toLower(assetName);

    // Some calls return a zero-row/zero-column default placeholder for the same asset name.
    // Returning that placeholder to Arena can break table_get* callers even though an earlier
    // valid table instance was already overlaid. Reuse the last known-good table instead.
    if (table->rowCount <= 0 || table->columnCount <= 1) {
        auto good = g_lastGoodTableByAsset.find(assetKey);
        if (good != g_lastGoodTableByAsset.end() && good->second) {
            if (!g_loggedFallbackByAsset[assetKey]) {
                g_loggedFallbackByAsset[assetKey] = true;
                log("Using cached valid string_table for '%s' instead of invalid placeholder (%d rows, %d cols, placeholder=%p, cached=%p)",
                    assetName.c_str(), table->rowCount, table->columnCount, static_cast<void*>(table), static_cast<void*>(good->second));
            }
            return good->second;
        }

        if (!g_loggedInvalidShapeByAsset[assetKey]) {
            g_loggedInvalidShapeByAsset[assetKey] = true;
            log("Refusing loose string_table '%s': target table has invalid shape (%d rows, %d cols); no cached valid table exists yet",
                assetName.c_str(), table->rowCount, table->columnCount);
        }
        return table;
    }

    if (isGunsmithVariantTableName(assetName)) {
        auto blueprintLoose = findLooseAssetFile("mp/classtable_arena_blueprints.csv");
        if (!blueprintLoose) {
            static bool loggedMissingBlueprintSource = false;
            if (!loggedMissingBlueprintSource) {
                loggedMissingBlueprintSource = true;
                log("Gunsmith variant table '%s' requested, but no loose mp/classtable_arena_blueprints.csv source exists under watched assets roots",
                    assetName.c_str());
            }
            return table;
        }

        fs::file_time_type lastWrite{};
        try {
            lastWrite = fs::last_write_time(*blueprintLoose);
        } catch (...) {
            log("Could not read timestamp for blueprint source string_table '%s'", blueprintLoose->string().c_str());
            return table;
        }

        auto activeIt = g_active.find(activeKey);
        if (activeIt != g_active.end() && activeIt->second && activeIt->second->lastWrite == lastWrite && activeIt->second->target == table) {
            return table;
        }

        auto csv = parseCsv(*blueprintLoose);
        if (!csv) {
            log("Failed to parse blueprint source string_table '%s' for gunsmith variant attachment overlay",
                blueprintLoose->string().c_str());
            return table;
        }

        const int oldRows = table->rowCount;
        const int oldCols = table->columnCount;
        if (replaceGunsmithVariantTable(table, assetName, *csv, lastWrite)) {
            g_lastGoodTableByAsset[assetKey] = table;
            log("Overlayed gunsmith variant attachments for '%s' from '%s' (source %llu rows x %llu cols -> target %d rows x %d cols, table=%p)",
                assetName.c_str(),
                blueprintLoose->string().c_str(),
                static_cast<unsigned long long>(csv->rows.size()),
                static_cast<unsigned long long>(csv->cols),
                oldRows,
                oldCols,
                static_cast<void*>(table));
        }

        return table;
    }

    auto loose = findLooseAssetFile(assetName);
    if (!loose) {
        static std::unordered_map<std::string, bool> loggedMissing;
        if (!loggedMissing[assetKey]) {
            loggedMissing[assetKey] = true;
            log("Target string_table '%s' requested, but no loose file exists under watched assets roots", assetName.c_str());
        }
        return table;
    }

    fs::file_time_type lastWrite{};
    try {
        lastWrite = fs::last_write_time(*loose);
    } catch (...) {
        log("Could not read timestamp for loose string_table '%s'", loose->string().c_str());
        return table;
    }

    auto activeIt = g_active.find(activeKey);
    if (activeIt != g_active.end() && activeIt->second && activeIt->second->lastWrite == lastWrite && activeIt->second->target == table) {
        return table; // Already patched this exact table instance with this exact file version.
    }

    auto csv = parseCsv(*loose);
    if (!csv) {
        log("Failed to parse loose string_table '%s'", loose->string().c_str());
        return table;
    }

    const int oldRows = table->rowCount;
    const int oldCols = table->columnCount;
    if ((isGenericOspWeaponTable(assetName) ? replaceGenericStringTable(table, assetName, *csv, lastWrite) : replaceStringTable(table, assetName, *csv, lastWrite))) {
        g_lastGoodTableByAsset[assetKey] = table;
        log("Overlayed loose string_table '%s' from '%s' (source %llu rows x %llu cols -> target %d rows x %d cols, table=%p)",
            assetName.c_str(),
            loose->string().c_str(),
            static_cast<unsigned long long>(csv->rows.size()),
            static_cast<unsigned long long>(csv->cols),
            oldRows,
            oldCols,
            static_cast<void*>(table));
    }

    return table;
}


XAssetHeader __fastcall hkDBFindXAssetHeader(int type, const char* name, int allowCreateDefault) {
    if (!g_previousDBFind) {
        XAssetHeader empty{};
        return empty;
    }

    // Guard against accidental recursion. If the chained iw8-mod/version hook is sane,
    // this should not trigger. If it does, call through without our replacement logic.
    if (g_inHook) {
        return g_previousDBFind(type, name, allowCreateDefault);
    }

    g_inHook = true;
    XAssetHeader result{};

    result = g_previousDBFind(type, name, allowCreateDefault);

    logScriptProbeAsset(type, name, allowCreateDefault, result);

    if (type == ASSET_TYPE_STRINGTABLE && result.stringTable && name && isTargetStringTable(name)) {
        try {
            result.stringTable = tryReplaceStringTable(name, result.stringTable);
        } catch (...) {
            log("C++ exception while replacing string_table '%s'", name ? name : "<null>");
        }
    }

    g_inHook = false;
    return result;
}

void* decodeExistingJump(void* target) {
    auto* p = static_cast<std::uint8_t*>(target);

    // E9 rel32
    if (p[0] == 0xE9) {
        const auto rel = *reinterpret_cast<std::int32_t*>(p + 1);
        return p + 5 + rel;
    }

    // FF 25 disp32  => jmp qword ptr [rip+disp32]
    if (p[0] == 0xFF && p[1] == 0x25) {
        const auto disp = *reinterpret_cast<std::int32_t*>(p + 2);
        auto** slot = reinterpret_cast<void**>(p + 6 + disp);
        return *slot;
    }

    // 48 B8 imm64 FF E0 => mov rax, imm64; jmp rax
    if (p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0) {
        return *reinterpret_cast<void**>(p + 2);
    }

    return nullptr;
}


void* resolveRel32CallTarget(std::uint8_t* callSite) {
    if (!callSite) return nullptr;
    if (callSite[0] != 0xE8) return nullptr;
    const auto rel = *reinterpret_cast<std::int32_t*>(callSite + 1);
    return callSite + 5 + rel;
}

std::string firstBytesHex(void* address, std::size_t count) {
    char buf[256]{};
    char* out = buf;
    auto* p = static_cast<unsigned char*>(address);
    for (std::size_t i = 0; i < count && (out - buf) < static_cast<ptrdiff_t>(sizeof(buf) - 4); ++i) {
        sprintf_s(out, sizeof(buf) - (out - buf), "%02X ", p[i]);
        out += strlen(out);
    }
    return std::string(buf);
}


void* allocateExecutableNear(void* target, std::size_t size) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);

    const auto granularity = static_cast<std::uintptr_t>(si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000);
    const auto targetAddr = reinterpret_cast<std::uintptr_t>(target);
    const auto maxDistance = static_cast<std::uintptr_t>(0x7FFF0000ULL);

    auto alignedDown = [&](std::uintptr_t value) -> std::uintptr_t {
        return value & ~(granularity - 1);
    };

    for (std::uintptr_t distance = 0; distance < maxDistance; distance += granularity) {
        // Try above, then below. Above first tends to find nearby gaps after module mappings.
        for (int dir = 0; dir < 2; ++dir) {
            std::uintptr_t candidate = 0;

            if (dir == 0) {
                if (targetAddr + distance < targetAddr) continue;
                candidate = alignedDown(targetAddr + distance);
            } else {
                if (targetAddr < distance) continue;
                candidate = alignedDown(targetAddr - distance);
            }

            // Make sure the original E9 rel32 can reach this stub.
            const auto rel64 = static_cast<std::int64_t>(candidate) -
                static_cast<std::int64_t>(targetAddr + 5);
            if (rel64 < (std::numeric_limits<std::int32_t>::min)() ||
                rel64 > (std::numeric_limits<std::int32_t>::max)()) {
                continue;
            }

            void* allocated = VirtualAlloc(
                reinterpret_cast<void*>(candidate),
                size,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_EXECUTE_READWRITE
            );

            if (allocated) {
                return allocated;
            }
        }
    }

    return nullptr;
}

void* makeAbsoluteJumpStub(void* nearTarget, void* destination) {
    // mov rax, imm64
    // jmp rax
    static constexpr std::size_t stubSize = 16;
    auto* stub = static_cast<std::uint8_t*>(allocateExecutableNear(nearTarget, stubSize));
    if (!stub) {
        return nullptr;
    }

    stub[0] = 0x48;
    stub[1] = 0xB8;
    *reinterpret_cast<void**>(stub + 2) = destination;
    stub[10] = 0xFF;
    stub[11] = 0xE0;
    for (std::size_t i = 12; i < stubSize; ++i) {
        stub[i] = 0xCC;
    }

    FlushInstructionCache(GetCurrentProcess(), stub, stubSize);
    return stub;
}


bool installHook() {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) {
        log("GetModuleHandleW(nullptr) failed");
        return false;
    }

    auto* callSite = reinterpret_cast<std::uint8_t*>(exe) + DB_FIND_XASSETHEADER_CALLSITE_RVA_141;

    log("DB_FindXAssetHeader signature cache match/callsite: ModernWarfare.exe+0x%llX -> %p",
        static_cast<unsigned long long>(DB_FIND_XASSETHEADER_CALLSITE_RVA_141),
        callSite);
    log("Callsite first 16 bytes: %s", firstBytesHex(callSite, 16).c_str());

    auto* target = static_cast<std::uint8_t*>(resolveRel32CallTarget(callSite));
    if (!target) {
        log("Cache RVA does not point at an E8 rel32 call. Cannot resolve DB_FindXAssetHeader target safely.");
        return false;
    }

    g_dbFindEntry = target;
    log("Resolved DB_FindXAssetHeader target via Add(1).Rip(): %p", target);
    log("Target first 16 bytes before hook: %s", firstBytesHex(target, 16).c_str());

    void* previous = decodeExistingJump(target);
    if (!previous) {
        log("DB_FindXAssetHeader target does not currently look like an existing jump hook. Refusing to patch without a full trampoline.");
        log("This usually means iw8-mod/version.dll has not hooked it yet, or the target RVA is wrong.");
        log("Inject at the main menu/lobby after iw8-mod has finished installing hooks.");
        return false;
    }

    g_previousDBFind = reinterpret_cast<DB_FindXAssetHeader_t>(previous);
    log("Chaining previous DB_FindXAssetHeader handler at %p", previous);

    // The target already has a 5-byte E9 hook installed by iw8-mod/version.dll.
    // A direct E9 from ModernWarfare.exe to this sidecar DLL may be outside rel32 range.
    // So allocate a tiny executable stub near the game function. The game function jumps
    // to that nearby stub using a 5-byte E9, and the stub does an absolute jump to our hook.
    auto* hookAddress = reinterpret_cast<std::uint8_t*>(&hkDBFindXAssetHeader);
    void* stub = makeAbsoluteJumpStub(target, hookAddress);
    if (!stub) {
        log("Failed to allocate nearby executable jump stub within rel32 range of target=%p", target);
        return false;
    }

    const auto rel64 = reinterpret_cast<std::intptr_t>(stub) -
        (reinterpret_cast<std::intptr_t>(target) + 5);

    if (rel64 < (std::numeric_limits<std::int32_t>::min)() ||
        rel64 > (std::numeric_limits<std::int32_t>::max)()) {
        log("Internal error: allocated stub is outside rel32 range. target=%p stub=%p rel=%lld",
            target, stub, static_cast<long long>(rel64));
        return false;
    }

    log("Allocated near jump stub at %p -> hook %p", stub, hookAddress);

    std::uint8_t patch[5]{};
    patch[0] = 0xE9;
    *reinterpret_cast<std::int32_t*>(patch + 1) = static_cast<std::int32_t>(rel64);

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        log("VirtualProtect failed while installing 5-byte near-stub hook: %lu", GetLastError());
        return false;
    }

    memcpy(target, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));

    DWORD ignored = 0;
    VirtualProtect(target, sizeof(patch), oldProtect, &ignored);

    log("Target first 16 bytes after hook: %s", firstBytesHex(target, 16).c_str());
    log("OVERLAY_CYCLIC_EXPANDED_POOL_OSP_BLUEPRINT_GUNSMITH_ATTACH_TAKEFISTS_MEMPATCH_BUILD_ID 2026-06-25");
    log("5-byte near-stub chain hook installed successfully. Arena classTables, mp/arenaGGWeapons(.csv/.alt), and matching mp/gunsmith/*_variants.csv attachment cells can be overlay-patched.");
    return true;
}

void initPaths() {
    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(g_module, modulePath, MAX_PATH)) {
        fs::path p(modulePath);
        g_sidecarDir = p.parent_path();
    } else {
        g_sidecarDir = fs::current_path();
    }

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
        fs::path p(exePath);
        g_gameRoot = p.parent_path();
    } else {
        g_gameRoot = fs::current_path();
    }

    g_assetRoots.clear();
    g_assetRoots.emplace_back(g_gameRoot / ".iw8-mod" / "assets");
    g_assetRoots.emplace_back(g_gameRoot / "iw8-mod" / "assets");
    g_assetRoots.emplace_back(g_sidecarDir / ".iw8-mod" / "assets");
    g_assetRoots.emplace_back(g_sidecarDir / "iw8-mod" / "assets");
    g_assetRoots.emplace_back(g_sidecarDir / "assets");
}

DWORD WINAPI workerThread(LPVOID) {
    log("Sidecar worker thread entered");
    initPaths();

    log("Sidecar DLL dir: %s", narrow(g_sidecarDir.wstring()).c_str());
    log("ModernWarfare.exe dir: %s", narrow(g_gameRoot.wstring()).c_str());
    for (const auto& root : g_assetRoots) {
        log("Watching assets root: %s", narrow(root.wstring()).c_str());
    }

    log("Overlay/cyclic expanded-pool-index + OSP Blueprints full-sniper/gunsmith-attachment mode enabled. ClassTables, arenaGGWeapons/alt, and matching gunsmith variants can be overlay-patched.");
    log("Target string_table: mp/classtable_arena.csv");
    log("Target string_table: mp/classtable_arena_alt.csv");
    log("Target string_table: mp/classtable_arena_blueprints.csv");
    log("Target string_table: mp/arenaGGWeapons.csv");
    log("Target string_table: mp/arenaGGWeapons_alt.csv");
    log("Target string_table pattern: mp/gunsmith/*_variants.csv (attachments synthesized from mp/classtable_arena_blueprints.csv)");

    // Let iw8-mod finish its startup hooks if this is injected very early.
    Sleep(1500);

    installFileProbeHooks();

    g_hookInstalled = installHook();
    g_hookInstallFailed = !g_hookInstalled;

    HANDLE takefistsThread = CreateThread(nullptr, 0, takefistsPatchThread, nullptr, 0, nullptr);
    if (takefistsThread) {
        log("Takefists memory patch thread created");
        CloseHandle(takefistsThread);
    } else {
        log("Takefists memory patch CreateThread failed: %lu", GetLastError());
    }

    return 0;
}

} // namespace iw8st

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    using namespace iw8st;

    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        initLogPath(module);
        log("DllMain DLL_PROCESS_ATTACH entered");

        HANDLE thread = CreateThread(nullptr, 0, workerThread, nullptr, 0, nullptr);
        if (thread) {
            log("Worker thread created");
            CloseHandle(thread);
        } else {
            log("CreateThread failed: %lu", GetLastError());
        }
    }

    return TRUE;
}
