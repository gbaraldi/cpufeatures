// Host CPU detection for AArch64.
// Standalone - no LLVM dependency.
// Supports Linux (/proc/cpuinfo), macOS (sysctlbyname), Windows (stubs).

#include "target_tables_aarch64.h"
#include "target_parsing.h"
#include "target_internal.h"

#include <string>
#include <cstring>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fstream>
#endif

// ============================================================================
// macOS: CPU detection via sysctlbyname
// ============================================================================

#if defined(__APPLE__)

#define CPUFAMILY_ARM_FIRESTORM_ICESTORM 0x1b588bb3
#define CPUFAMILY_ARM_BLIZZARD_AVALANCHE 0xda33d83d
#define CPUFAMILY_ARM_EVEREST_SAWTOOTH   0x8765edea
#define CPUFAMILY_ARM_IBIZA              0xfa33415e
#define CPUFAMILY_ARM_PALMA              0x72015832
#define CPUFAMILY_ARM_LOBOS              0x5f4dea93
#define CPUFAMILY_ARM_COLL               0x2876f5b5
#define CPUFAMILY_ARM_DONAN              0x6f5129ac
#define CPUFAMILY_ARM_BRAVA              0x17d5b93a
#define CPUFAMILY_ARM_TAHITI             0x75d4acb9
#define CPUFAMILY_ARM_TUPAI              0x204526d0

const char *tp_get_host_cpu_name(void) {
    static std::string cpu_name;
    if (!cpu_name.empty()) return cpu_name.c_str();

    uint32_t family = 0;
    size_t len = sizeof(family);
    sysctlbyname("hw.cpufamily", &family, &len, NULL, 0);

    const char *name;
    switch (family) {
    case CPUFAMILY_ARM_FIRESTORM_ICESTORM: name = "apple-a14"; break;
    case CPUFAMILY_ARM_BLIZZARD_AVALANCHE: name = "apple-a15"; break;
    case CPUFAMILY_ARM_EVEREST_SAWTOOTH:
    case CPUFAMILY_ARM_IBIZA:
    case CPUFAMILY_ARM_PALMA:
    case CPUFAMILY_ARM_LOBOS:
        name = "apple-a16"; break;
    case CPUFAMILY_ARM_COLL:
        name = "apple-a17"; break;
    case CPUFAMILY_ARM_DONAN:
    case CPUFAMILY_ARM_BRAVA:
    case CPUFAMILY_ARM_TAHITI:
    case CPUFAMILY_ARM_TUPAI:
        name = "apple-m4"; break;
    default:
        name = "apple-m4"; break;
    }

    if (!find_cpu(name))
        name = "generic";

    cpu_name = name;
    return cpu_name.c_str();
}

void tp_get_host_features(FeatureBits *features) {
    std::memset(features, 0, sizeof(FeatureBits));

    const char *cpu = tp_get_host_cpu_name();
    const CPUEntry *entry = find_cpu(cpu);
    if (entry)
        std::memcpy(features, &entry->features, sizeof(FeatureBits));
}

// ============================================================================
// Windows AArch64: CPU detection
// ============================================================================

#elif defined(_WIN32)

const char *tp_get_host_cpu_name(void) {
    return "generic";
}

void tp_get_host_features(FeatureBits *features) {
    std::memset(features, 0, sizeof(FeatureBits));

    const FeatureEntry *fe;

    if ((fe = find_feature("neon"))) feature_set(features, fe->bit);
    if ((fe = find_feature("fp-armv8"))) feature_set(features, fe->bit);

    #ifndef PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE
    #define PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE 31
    #endif
    if (IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE)) {
        if ((fe = find_feature("crc"))) feature_set(features, fe->bit);
    }

    #ifndef PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE
    #define PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE 30
    #endif
    if (IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE)) {
        if ((fe = find_feature("aes"))) feature_set(features, fe->bit);
        if ((fe = find_feature("sha2"))) feature_set(features, fe->bit);
    }

    #ifndef PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE
    #define PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE 34
    #endif
    if (IsProcessorFeaturePresent(PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE)) {
        if ((fe = find_feature("lse"))) feature_set(features, fe->bit);
    }

    #ifndef PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE
    #define PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE 43
    #endif
    if (IsProcessorFeaturePresent(PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE)) {
        if ((fe = find_feature("dotprod"))) feature_set(features, fe->bit);
    }

    expand_implied(features);
}

// ============================================================================
// Linux AArch64: CPU detection via /proc/cpuinfo
// ============================================================================

#else // Linux

// Implementer + part -> LLVM CPU name table
struct ArmCPUInfo {
    unsigned implementer;
    unsigned part;
    const char *name;
};

static const ArmCPUInfo arm_cpus[] = {
    {0x41, 0xd03, "cortex-a53"},
    {0x41, 0xd04, "cortex-a35"},
    {0x41, 0xd05, "cortex-a55"},
    {0x41, 0xd06, "cortex-a65"},
    {0x41, 0xd07, "cortex-a57"},
    {0x41, 0xd08, "cortex-a72"},
    {0x41, 0xd09, "cortex-a73"},
    {0x41, 0xd0a, "cortex-a75"},
    {0x41, 0xd0b, "cortex-a76"},
    {0x41, 0xd0c, "neoverse-n1"},
    {0x41, 0xd0d, "cortex-a77"},
    {0x41, 0xd40, "neoverse-v1"},
    {0x41, 0xd41, "cortex-a78"},
    {0x41, 0xd44, "cortex-x1"},
    {0x41, 0xd46, "cortex-a510"},
    {0x41, 0xd47, "cortex-a710"},
    {0x41, 0xd48, "cortex-x2"},
    {0x41, 0xd49, "neoverse-n2"},
    {0x41, 0xd4d, "cortex-a715"},
    {0x41, 0xd4e, "cortex-x3"},
    {0x41, 0xd4f, "neoverse-v2"},
    {0x41, 0xd80, "cortex-a520"},
    {0x41, 0xd81, "cortex-a720"},
    {0x41, 0xd82, "cortex-x4"},
    {0x41, 0xd84, "neoverse-v3"},
    {0x41, 0xd85, "cortex-x925"},
    {0x41, 0xd87, "cortex-a725"},
    {0x61, 0x022, "apple-m1"},
    {0x61, 0x023, "apple-m1"},
    {0x61, 0x030, "apple-m2"},
    {0x61, 0x031, "apple-m2"},
    {0x61, 0x038, "apple-m3"},
    {0x61, 0x039, "apple-m3"},
    {0x61, 0x049, "apple-m4"},
    {0x51, 0x001, "oryon-1"},
    {0xc0, 0xac3, "ampere1"},
    {0xc0, 0xac4, "ampere1a"},
    {0xc0, 0xac5, "ampere1b"},
    {0, 0, nullptr}
};

// Load and cache /proc/cpuinfo content
static const std::string &load_cpuinfo() {
    static std::string content;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream f("/proc/cpuinfo");
        if (f) {
            content.assign(std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>());
        }
    }
    return content;
}

// Extract a field value from cpuinfo text
static std::string_view cpuinfo_field(std::string_view buf, std::string_view field) {
    size_t pos = 0;
    while (pos < buf.size()) {
        auto found = buf.find(field, pos);
        if (found == std::string_view::npos) break;

        // Must be at start of line
        if (found > 0 && buf[found - 1] != '\n') {
            pos = found + 1;
            continue;
        }

        auto after = found + field.size();
        // Skip whitespace then ':'
        while (after < buf.size() && (buf[after] == ' ' || buf[after] == '\t'))
            after++;
        if (after >= buf.size() || buf[after] != ':') {
            pos = found + 1;
            continue;
        }
        after++; // skip ':'
        while (after < buf.size() && (buf[after] == ' ' || buf[after] == '\t'))
            after++;

        // Find end of line
        auto eol = buf.find('\n', after);
        if (eol == std::string_view::npos) eol = buf.size();
        return buf.substr(after, eol - after);
    }
    return {};
}

const char *tp_get_host_cpu_name(void) {
    static std::string cpu_name;
    if (!cpu_name.empty()) return cpu_name.c_str();

    const auto &info = load_cpuinfo();
    auto impl_sv = cpuinfo_field(info, "CPU implementer");
    auto part_sv = cpuinfo_field(info, "CPU part");

    const char *name = "generic";
    if (!impl_sv.empty() && !part_sv.empty()) {
        unsigned impl = static_cast<unsigned>(std::strtoul(
            std::string(impl_sv).c_str(), nullptr, 0));
        unsigned part = static_cast<unsigned>(std::strtoul(
            std::string(part_sv).c_str(), nullptr, 0));

        for (const ArmCPUInfo *c = arm_cpus; c->name; c++) {
            if (c->implementer == impl && c->part == part) {
                name = c->name;
                break;
            }
        }
    }

    if (!find_cpu(name))
        name = "generic";

    cpu_name = name;
    return cpu_name.c_str();
}

// Linux feature name -> LLVM feature name mapping
struct FeatureMap {
    const char *linux_name;
    const char *llvm_name;
};

static const FeatureMap aarch64_feature_map[] = {
    {"asimd", "neon"},
    {"fp", "fp-armv8"},
    {"crc32", "crc"},
    {"atomics", "lse"},
    {"rng", "rand"},
    {"sha3", "sha3"},
    {"sm4", "sm4"},
    {"sve", "sve"},
    {"sve2", "sve2"},
    {"sveaes", "sve-aes"},
    {"svesha3", "sve-sha3"},
    {"svesm4", "sve-sm4"},
    {"dotprod", "dotprod"},
    {"bf16", "bf16"},
    {"i8mm", "i8mm"},
    {"fphp", "fullfp16"},
    {"ssbs", "ssbs"},
    {"sb", "sb"},
    {"dcpop", "rcpc"},
    {"flagm", "flagm"},
    {"dit", "dit"},
    {"bti", "bti"},
    {"paca", "pauth"},
    {nullptr, nullptr}
};

void tp_get_host_features(FeatureBits *features) {
    std::memset(features, 0, sizeof(FeatureBits));

    const auto &info = load_cpuinfo();
    auto feat_line = cpuinfo_field(info, "Features");
    if (feat_line.empty()) return;

    bool has_aes = false, has_pmull = false, has_sha1 = false, has_sha2 = false;

    // Tokenize using string_view (no strtok, no mutation)
    auto tokens = split(feat_line, ' ');
    for (auto tok : tokens) {
        if (tok == "aes") has_aes = true;
        else if (tok == "pmull") has_pmull = true;
        else if (tok == "sha1") has_sha1 = true;
        else if (tok == "sha2") has_sha2 = true;

        // Map Linux name -> LLVM name
        for (const FeatureMap *m = aarch64_feature_map; m->linux_name; m++) {
            if (tok == m->linux_name) {
                const FeatureEntry *fe = find_feature(m->llvm_name);
                if (fe) feature_set(features, fe->bit);
                break;
            }
        }
    }

    if (has_aes && has_pmull) {
        const FeatureEntry *fe = find_feature("aes");
        if (fe) feature_set(features, fe->bit);
    }

    if (has_sha1 && has_sha2) {
        const FeatureEntry *fe = find_feature("sha2");
        if (fe) feature_set(features, fe->bit);
    }

    expand_implied(features);
}

#endif // platform
