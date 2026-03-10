// Host CPU detection for x86_64.
// Standalone - no LLVM dependency.
// Uses CPUID to detect CPU name and features.

#include "target_tables_x86_64.h"
#include "target_parsing.h"

#include <string.h>
#include <cpuid.h>

// ============================================================================
// CPUID helpers
// ============================================================================

struct CPUIDResult {
    unsigned eax, ebx, ecx, edx;
};

static CPUIDResult cpuid(unsigned leaf, unsigned subleaf = 0) {
    CPUIDResult r = {};
    __cpuid_count(leaf, subleaf, r.eax, r.ebx, r.ecx, r.edx);
    return r;
}

static unsigned cpuid_max_leaf() {
    return cpuid(0).eax;
}

static unsigned cpuid_max_ext_leaf() {
    return cpuid(0x80000000).eax;
}

// ============================================================================
// CPU vendor/family/model detection
// ============================================================================

enum Vendor { VENDOR_INTEL, VENDOR_AMD, VENDOR_OTHER };

static Vendor get_vendor() {
    auto r = cpuid(0);
    // "GenuineIntel"
    if (r.ebx == 0x756e6547 && r.edx == 0x49656e69 && r.ecx == 0x6c65746e)
        return VENDOR_INTEL;
    // "AuthenticAMD"
    if (r.ebx == 0x68747541 && r.edx == 0x69746e65 && r.ecx == 0x444d4163)
        return VENDOR_AMD;
    return VENDOR_OTHER;
}

struct CPUModel {
    unsigned family;
    unsigned model;
    unsigned stepping;
};

static CPUModel get_cpu_model() {
    auto r = cpuid(1);
    CPUModel m;
    m.stepping = r.eax & 0xf;
    m.model = (r.eax >> 4) & 0xf;
    m.family = (r.eax >> 8) & 0xf;

    if (m.family == 6 || m.family == 0xf) {
        m.model += ((r.eax >> 16) & 0xf) << 4;
    }
    if (m.family == 0xf) {
        m.family += (r.eax >> 20) & 0xff;
    }
    return m;
}

// ============================================================================
// Host CPU name detection
// Maps family/model to LLVM CPU name strings that match our tables.
// This is essentially what LLVM's Host.cpp does.
// ============================================================================

static const char *detect_intel_cpu(const CPUModel &m) {
    // Intel family 6
    if (m.family != 6) return "generic";

    switch (m.model) {
    case 0x0f: // Core 2
    case 0x16: return "core2";
    case 0x17:
    case 0x1d: return "core2"; // Penryn
    case 0x1a:
    case 0x1e:
    case 0x1f:
    case 0x2e: return "nehalem";
    case 0x25:
    case 0x2c:
    case 0x2f: return "westmere";
    case 0x2a:
    case 0x2d: return "sandybridge";
    case 0x3a:
    case 0x3e: return "ivybridge";
    case 0x3c:
    case 0x3f:
    case 0x45:
    case 0x46: return "haswell";
    case 0x3d:
    case 0x47:
    case 0x4f:
    case 0x56: return "broadwell";
    case 0x4e:
    case 0x5e:
    case 0x8e:
    case 0x9e: return "skylake";
    case 0x55: {
        // Skylake-X / Cascade Lake / Cooper Lake
        if (m.stepping >= 5) return "cascadelake";
        return "skylake-avx512";
    }
    case 0x66: return "cannonlake";
    case 0x6a:
    case 0x6c: return "icelake-server";
    case 0x7d:
    case 0x7e: return "icelake-client";
    case 0x8c:
    case 0x8d: return "tigerlake";
    case 0x8f: return "sapphirerapids";
    case 0x97:
    case 0x9a: return "alderlake";
    case 0xb7:
    case 0xba:
    case 0xbf: return "raptorlake";
    case 0xaa:
    case 0xac: return "meteorlake";
    case 0xbd: return "lunarlake";
    case 0xc5: return "arrowlake";
    case 0xc6: return "arrowlake-s";
    case 0xcf: return "emeraldrapids";
    case 0xad:
    case 0xae: return "graniterapids";
    default: return "generic";
    }
}

static const char *detect_amd_cpu(const CPUModel &m) {
    switch (m.family) {
    case 0x10: return "amdfam10";
    case 0x14: return "btver1";
    case 0x15:
        if (m.model >= 0x60) return "bdver4";
        if (m.model >= 0x30) return "bdver3";
        if (m.model >= 0x02) return "bdver2";
        return "bdver1";
    case 0x16: return "btver2";
    case 0x17:
        if (m.model >= 0x30) return "znver2";
        return "znver1";
    case 0x19:
        if (m.model >= 0x10) return "znver4";
        return "znver3";
    case 0x1a: return "znver5";
    default: return "generic";
    }
}

const char *tp_get_host_cpu_name(void) {
    static char cpu_name[128] = {};
    if (cpu_name[0]) return cpu_name;

    Vendor v = get_vendor();
    CPUModel m = get_cpu_model();

    const char *name;
    if (v == VENDOR_INTEL)
        name = detect_intel_cpu(m);
    else if (v == VENDOR_AMD)
        name = detect_amd_cpu(m);
    else
        name = "generic";

    // Verify it's in our table; if not, fall back to generic
    if (!find_cpu(name))
        name = "generic";

    strncpy(cpu_name, name, sizeof(cpu_name) - 1);
    return cpu_name;
}

// ============================================================================
// Host feature detection via CPUID
// Maps CPUID bits to LLVM feature names.
// ============================================================================

void tp_get_host_features(FeatureBits *features) {
    memset(features, 0, sizeof(FeatureBits));

    unsigned max_leaf = cpuid_max_leaf();
    unsigned max_ext = cpuid_max_ext_leaf();

    // Always have these on x86_64
    const FeatureEntry *f;
    if ((f = find_feature("64bit"))) feature_set(features, f->bit);
    if ((f = find_feature("cx8"))) feature_set(features, f->bit);
    if ((f = find_feature("cmov"))) feature_set(features, f->bit);
    if ((f = find_feature("fxsr"))) feature_set(features, f->bit);
    if ((f = find_feature("mmx"))) feature_set(features, f->bit);
    if ((f = find_feature("sse"))) feature_set(features, f->bit);
    if ((f = find_feature("sse2"))) feature_set(features, f->bit);
    if ((f = find_feature("x87"))) feature_set(features, f->bit);

    // CPUID leaf 1
    if (max_leaf >= 1) {
        auto r = cpuid(1);
        // ECX bits
        if (r.ecx & (1 << 0))  { if ((f = find_feature("sse3"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 1))  { if ((f = find_feature("pclmul"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 9))  { if ((f = find_feature("ssse3"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 12)) { if ((f = find_feature("fma"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 13)) { if ((f = find_feature("cx16"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 19)) { if ((f = find_feature("sse4.1"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 20)) { if ((f = find_feature("sse4.2"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 20)) { if ((f = find_feature("crc32"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 22)) { if ((f = find_feature("movbe"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 23)) { if ((f = find_feature("popcnt"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 25)) { if ((f = find_feature("aes"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 26)) { if ((f = find_feature("xsave"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 28)) { if ((f = find_feature("avx"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 29)) { if ((f = find_feature("f16c"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 30)) { if ((f = find_feature("rdrnd"))) feature_set(features, f->bit); }
    }

    // CPUID leaf 7, subleaf 0
    if (max_leaf >= 7) {
        auto r = cpuid(7, 0);
        // EBX bits
        if (r.ebx & (1 << 0))  { if ((f = find_feature("fsgsbase"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 3))  { if ((f = find_feature("bmi"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 5))  { if ((f = find_feature("avx2"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 8))  { if ((f = find_feature("bmi2"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 10)) { if ((f = find_feature("invpcid"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 16)) { if ((f = find_feature("avx512f"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 17)) { if ((f = find_feature("avx512dq"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 18)) { if ((f = find_feature("rdseed"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 19)) { if ((f = find_feature("adx"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 21)) { if ((f = find_feature("avx512ifma"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 23)) { if ((f = find_feature("clflushopt"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 24)) { if ((f = find_feature("clwb"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 28)) { if ((f = find_feature("avx512cd"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 29)) { if ((f = find_feature("sha"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 30)) { if ((f = find_feature("avx512bw"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 31)) { if ((f = find_feature("avx512vl"))) feature_set(features, f->bit); }

        // ECX bits
        if (r.ecx & (1 << 1))  { if ((f = find_feature("avx512vbmi"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 4))  { if ((f = find_feature("pku"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 5))  { if ((f = find_feature("waitpkg"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 6))  { if ((f = find_feature("avx512vbmi2"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 7))  { if ((f = find_feature("shstk"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 8))  { if ((f = find_feature("gfni"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 9))  { if ((f = find_feature("vaes"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 10)) { if ((f = find_feature("vpclmulqdq"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 11)) { if ((f = find_feature("avx512vnni"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 12)) { if ((f = find_feature("avx512bitalg"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 14)) { if ((f = find_feature("avx512vpopcntdq"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 22)) { if ((f = find_feature("rdpid"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 25)) { if ((f = find_feature("cldemote"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 27)) { if ((f = find_feature("movdiri"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 28)) { if ((f = find_feature("movdir64b"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 29)) { if ((f = find_feature("enqcmd"))) feature_set(features, f->bit); }

        // EDX bits
        if (r.edx & (1 << 5))  { if ((f = find_feature("uintr"))) feature_set(features, f->bit); }
        if (r.edx & (1 << 8))  { if ((f = find_feature("avx512vp2intersect"))) feature_set(features, f->bit); }
        if (r.edx & (1 << 14)) { if ((f = find_feature("serialize"))) feature_set(features, f->bit); }
        if (r.edx & (1 << 16)) { if ((f = find_feature("tsxldtrk"))) feature_set(features, f->bit); }
        if (r.edx & (1 << 18)) { if ((f = find_feature("pconfig"))) feature_set(features, f->bit); }
        if (r.edx & (1 << 22)) { if ((f = find_feature("amx-bf16"))) feature_set(features, f->bit); }
        if (r.edx & (1 << 23)) { if ((f = find_feature("avx512fp16"))) feature_set(features, f->bit); }
        if (r.edx & (1 << 24)) { if ((f = find_feature("amx-tile"))) feature_set(features, f->bit); }
        if (r.edx & (1 << 25)) { if ((f = find_feature("amx-int8"))) feature_set(features, f->bit); }
    }

    // CPUID leaf 7, subleaf 1
    if (max_leaf >= 7) {
        auto r = cpuid(7, 1);
        // EAX bits
        if (r.eax & (1 << 0))  { if ((f = find_feature("sha512"))) feature_set(features, f->bit); }
        if (r.eax & (1 << 1))  { if ((f = find_feature("sm3"))) feature_set(features, f->bit); }
        if (r.eax & (1 << 2))  { if ((f = find_feature("sm4"))) feature_set(features, f->bit); }
        if (r.eax & (1 << 4))  { if ((f = find_feature("avxvnni"))) feature_set(features, f->bit); }
        if (r.eax & (1 << 5))  { if ((f = find_feature("avx512bf16"))) feature_set(features, f->bit); }
        if (r.eax & (1 << 7))  { if ((f = find_feature("cmpccxadd"))) feature_set(features, f->bit); }
        if (r.eax & (1 << 21)) { if ((f = find_feature("amx-fp16"))) feature_set(features, f->bit); }
        if (r.eax & (1 << 23)) { if ((f = find_feature("avxifma"))) feature_set(features, f->bit); }

        // EBX bits
        if (r.ebx & (1 << 4))  { if ((f = find_feature("avxvnniint8"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 5))  { if ((f = find_feature("avxneconvert"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 8))  { if ((f = find_feature("amx-complex"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 10)) { if ((f = find_feature("avxvnniint16"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 14)) { if ((f = find_feature("prefetchi"))) feature_set(features, f->bit); }
    }

    // Extended CPUID 0x80000001
    if (max_ext >= 0x80000001) {
        auto r = cpuid(0x80000001);
        // ECX bits
        if (r.ecx & (1 << 0))  { if ((f = find_feature("sahf"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 5))  { if ((f = find_feature("lzcnt"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 6))  { if ((f = find_feature("sse4a"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 8))  { if ((f = find_feature("prfchw"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 11)) { if ((f = find_feature("xop"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 16)) { if ((f = find_feature("fma4"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 21)) { if ((f = find_feature("tbm"))) feature_set(features, f->bit); }
        if (r.ecx & (1 << 29)) { if ((f = find_feature("mwaitx"))) feature_set(features, f->bit); }
    }

    // XSAVE-related (CPUID leaf 0xD)
    if (max_leaf >= 0xd) {
        auto r = cpuid(0xd, 1);
        if (r.eax & (1 << 0)) { if ((f = find_feature("xsaveopt"))) feature_set(features, f->bit); }
        if (r.eax & (1 << 1)) { if ((f = find_feature("xsavec"))) feature_set(features, f->bit); }
        if (r.eax & (1 << 3)) { if ((f = find_feature("xsaves"))) feature_set(features, f->bit); }
    }

    // Extended CPUID 0x80000008
    if (max_ext >= 0x80000008) {
        auto r = cpuid(0x80000008);
        if (r.ebx & (1 << 0))  { if ((f = find_feature("clzero"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 4))  { if ((f = find_feature("rdpru"))) feature_set(features, f->bit); }
        if (r.ebx & (1 << 9))  { if ((f = find_feature("wbnoinvd"))) feature_set(features, f->bit); }
    }

    // AVX-512 implies evex512 (on CPUs that actually have 512-bit support)
    // This is set by the OS via XCR0 check, but for simplicity we check CPUID
    if ((f = find_feature("avx512f")) && feature_test(features, f->bit)) {
        if ((f = find_feature("evex512"))) feature_set(features, f->bit);
    }

    // Expand all implied features
    expand_implied(features);
}
