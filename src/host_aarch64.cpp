// Host CPU detection for AArch64.
// Standalone - no LLVM dependency.
// Supports Linux (/proc/cpuinfo), macOS (sysctlbyname), Windows (stubs).

#include "target_tables_aarch64.h"
#include "target_parsing.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(_WIN32)
// Windows AArch64 support - minimal for now
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// ============================================================================
// macOS: CPU detection via sysctlbyname
// ============================================================================

#if defined(__APPLE__)

// CPU family constants from mach/machine.h / XNU
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
    static char cpu_name[128] = {};
    if (cpu_name[0]) return cpu_name;

    uint32_t family = 0;
    size_t len = sizeof(family);
    sysctlbyname("hw.cpufamily", &family, &len, NULL, 0);

    const char *name;
    switch (family) {
    case CPUFAMILY_ARM_FIRESTORM_ICESTORM: name = "apple-m1"; break;
    case CPUFAMILY_ARM_BLIZZARD_AVALANCHE: name = "apple-m2"; break;
    case CPUFAMILY_ARM_EVEREST_SAWTOOTH:
    case CPUFAMILY_ARM_IBIZA:
    case CPUFAMILY_ARM_PALMA:
    case CPUFAMILY_ARM_LOBOS:
        name = "apple-m3"; break;
    case CPUFAMILY_ARM_COLL:
        name = "apple-a17"; break;
    case CPUFAMILY_ARM_DONAN:
    case CPUFAMILY_ARM_BRAVA:
    case CPUFAMILY_ARM_TAHITI:
    case CPUFAMILY_ARM_TUPAI:
        name = "apple-m4"; break;
    default:
        // Default to newest known Apple silicon
        name = "apple-m4"; break;
    }

    if (!find_cpu(name))
        name = "generic";

    strncpy(cpu_name, name, sizeof(cpu_name) - 1);
    return cpu_name;
}

void tp_get_host_features(FeatureBits *features) {
    memset(features, 0, sizeof(FeatureBits));

    // On Apple Silicon, just look up the CPU's features from our table.
    // Apple doesn't expose feature bits via CPUID - the CPU name is sufficient.
    const char *cpu = tp_get_host_cpu_name();
    const CPUEntry *entry = find_cpu(cpu);
    if (entry) {
        memcpy(features, &entry->features, sizeof(FeatureBits));
    }
}

// ============================================================================
// Windows AArch64: CPU detection
// ============================================================================

#elif defined(_WIN32)

const char *tp_get_host_cpu_name(void) {
    // Windows on ARM doesn't easily expose the CPU model.
    // For now, return generic. In the future we could use
    // IsProcessorFeaturePresent or the registry.
    return "generic";
}

void tp_get_host_features(FeatureBits *features) {
    memset(features, 0, sizeof(FeatureBits));

    // Detect features using IsProcessorFeaturePresent
    // These PF_ARM_* constants are from winnt.h
    const FeatureEntry *fe;

    // NEON is always available on Windows ARM64
    if ((fe = find_feature("neon"))) feature_set(features, fe->bit);
    if ((fe = find_feature("fp-armv8"))) feature_set(features, fe->bit);

    // CRC32
    #ifndef PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE
    #define PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE 31
    #endif
    if (IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE)) {
        if ((fe = find_feature("crc"))) feature_set(features, fe->bit);
    }

    // Crypto
    #ifndef PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE
    #define PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE 30
    #endif
    if (IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE)) {
        if ((fe = find_feature("aes"))) feature_set(features, fe->bit);
        if ((fe = find_feature("sha2"))) feature_set(features, fe->bit);
    }

    // Atomics (LSE)
    #ifndef PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE
    #define PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE 34
    #endif
    if (IsProcessorFeaturePresent(PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE)) {
        if ((fe = find_feature("lse"))) feature_set(features, fe->bit);
    }

    // Dot product
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

static char cpuinfo_buf[16384];
static int cpuinfo_loaded = 0;

static const char *load_cpuinfo(void) {
    if (cpuinfo_loaded) return cpuinfo_buf;
    cpuinfo_loaded = 1;
    cpuinfo_buf[0] = '\0';

    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return cpuinfo_buf;

    size_t n = fread(cpuinfo_buf, 1, sizeof(cpuinfo_buf) - 1, f);
    cpuinfo_buf[n] = '\0';
    fclose(f);
    return cpuinfo_buf;
}

static const char *cpuinfo_field(const char *buf, const char *field) {
    const char *p = buf;
    size_t flen = strlen(field);
    while ((p = strstr(p, field)) != NULL) {
        if (p == buf || *(p - 1) == '\n') {
            p += flen;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == ':') {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                return p;
            }
        }
        p++;
    }
    return NULL;
}

// Implementer + part -> LLVM CPU name table
struct ArmCPUInfo {
    unsigned implementer;
    unsigned part;
    const char *name;
};

static const ArmCPUInfo arm_cpus[] = {
    // ARM Ltd.
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
    // Apple (on Linux/Asahi)
    {0x61, 0x022, "apple-m1"},
    {0x61, 0x023, "apple-m1"},
    {0x61, 0x030, "apple-m2"},
    {0x61, 0x031, "apple-m2"},
    {0x61, 0x038, "apple-m3"},
    {0x61, 0x039, "apple-m3"},
    {0x61, 0x049, "apple-m4"},
    // Qualcomm
    {0x51, 0x001, "oryon-1"},
    // Ampere
    {0xc0, 0xac3, "ampere1"},
    {0xc0, 0xac4, "ampere1a"},
    {0xc0, 0xac5, "ampere1b"},
    {0, 0, NULL}
};

const char *tp_get_host_cpu_name(void) {
    static char cpu_name[128] = {};
    if (cpu_name[0]) return cpu_name;

    const char *info = load_cpuinfo();
    const char *impl_str = cpuinfo_field(info, "CPU implementer");
    const char *part_str = cpuinfo_field(info, "CPU part");

    const char *name = "generic";
    if (impl_str && part_str) {
        unsigned impl = (unsigned)strtoul(impl_str, NULL, 0);
        unsigned part = (unsigned)strtoul(part_str, NULL, 0);

        for (const ArmCPUInfo *c = arm_cpus; c->name; c++) {
            if (c->implementer == impl && c->part == part) {
                name = c->name;
                break;
            }
        }
    }

    if (!find_cpu(name))
        name = "generic";

    strncpy(cpu_name, name, sizeof(cpu_name) - 1);
    return cpu_name;
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
    {NULL, NULL}
};

void tp_get_host_features(FeatureBits *features) {
    memset(features, 0, sizeof(FeatureBits));

    const char *info = load_cpuinfo();
    const char *feat_line = cpuinfo_field(info, "Features");
    if (!feat_line) return;

    char line_buf[2048];
    const char *eol = feat_line;
    while (*eol && *eol != '\n') eol++;
    size_t len = eol - feat_line;
    if (len >= sizeof(line_buf)) len = sizeof(line_buf) - 1;
    memcpy(line_buf, feat_line, len);
    line_buf[len] = '\0';

    int has_aes = 0, has_pmull = 0, has_sha1 = 0, has_sha2 = 0;

    char *tok = strtok(line_buf, " \t");
    while (tok) {
        if (strcmp(tok, "aes") == 0) has_aes = 1;
        else if (strcmp(tok, "pmull") == 0) has_pmull = 1;
        else if (strcmp(tok, "sha1") == 0) has_sha1 = 1;
        else if (strcmp(tok, "sha2") == 0) has_sha2 = 1;

        const char *llvm_name = NULL;
        for (const FeatureMap *m = aarch64_feature_map; m->linux_name; m++) {
            if (strcmp(tok, m->linux_name) == 0) {
                llvm_name = m->llvm_name;
                break;
            }
        }

        if (llvm_name) {
            const FeatureEntry *fe = find_feature(llvm_name);
            if (fe) feature_set(features, fe->bit);
        }

        tok = strtok(NULL, " \t");
    }

    // AES requires both aes and pmull
    if (has_aes && has_pmull) {
        const FeatureEntry *fe = find_feature("aes");
        if (fe) feature_set(features, fe->bit);
    }

    // SHA2 requires both sha1 and sha2
    if (has_sha1 && has_sha2) {
        const FeatureEntry *fe = find_feature("sha2");
        if (fe) feature_set(features, fe->bit);
    }

    expand_implied(features);
}

#endif // platform
