// Standalone target parsing library.
// Uses CPU/feature tables generated at build time from LLVM's TableGen data.
// Zero LLVM runtime dependency.
//
// Usage: #include the generated table header first, then this header.
//   #include "target_tables_x86_64.h"
//   #include "target_parsing.h"

#ifndef TARGET_PARSING_H
#define TARGET_PARSING_H

#include <stdint.h>
#include <stddef.h>

// Verify that a generated table header was included first
#ifndef TARGET_FEATURE_WORDS
#error "Include the generated target table header before target_parsing.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Flags for target cloning behavior
enum {
    TF_VEC_CALL       = 1 << 0,
    TF_CLONE_ALL      = 1 << 1,
    TF_CLONE_MATH     = 1 << 2,
    TF_CLONE_LOOP     = 1 << 3,
    TF_CLONE_SIMD     = 1 << 4,
    TF_UNKNOWN_NAME   = 1 << 5,
    TF_OPTSIZE        = 1 << 6,
    TF_MINSIZE        = 1 << 7,
    TF_CLONE_CPU      = 1 << 8,
    TF_CLONE_FLOAT16  = 1 << 9,
    TF_CLONE_BFLOAT16 = 1 << 10,
};

#define MAX_TARGETS 16
#define MAX_EXTRA_FEATURES 64

// A parsed target from the target string
typedef struct {
    const char *cpu_name;
    uint32_t flags;
    int base;  // -1 = default
    const char *extra_features[MAX_EXTRA_FEATURES]; // "+feat" or "-feat"
    unsigned num_extra_features;
} ParsedTarget;

// A fully resolved target
typedef struct {
    char cpu_name[128];
    FeatureBits features;
    uint32_t flags;
    int base;
    char ext_features[512];
} ResolvedTarget;

// Target spec for LLVM codegen
typedef struct {
    char cpu_name[128];
    char cpu_features[2048];
    uint32_t flags;
    int base;
} TargetSpec;

// Parse a target string like "haswell;skylake,+avx512f,-sse4a"
int tp_parse_target_string(const char *target_str,
                           ParsedTarget *targets, int max_targets);

// Resolve parsed targets against the CPU/feature database
int tp_resolve_targets(const ParsedTarget *parsed, int num_parsed,
                       ResolvedTarget *resolved,
                       const FeatureBits *host_features,
                       const char *host_cpu);

// Compute clone flags for multi-versioned targets
void tp_compute_clone_flags(ResolvedTarget *targets, int num_targets);

// Match sysimage targets against host, return best match index
int tp_match_sysimg_target(const ResolvedTarget *targets, int num_targets,
                           const FeatureBits *host_features,
                           const char *host_cpu);

// Generate LLVM feature strings from resolved targets
int tp_get_target_specs(const ResolvedTarget *resolved, int num_resolved,
                        TargetSpec *specs);

// Build an LLVM-compatible feature string from a feature bitset
void tp_build_feature_string(const FeatureBits *features,
                             const FeatureBits *baseline,
                             char *buf, size_t bufsize);

// Host CPU detection
const char *tp_get_host_cpu_name(void);
void tp_get_host_features(FeatureBits *features);

#ifdef __cplusplus
}
#endif

#endif // TARGET_PARSING_H
