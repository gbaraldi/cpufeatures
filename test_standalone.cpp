// Test the standalone target parsing library - NO LLVM dependency!

// Include the right table for the host architecture
#if defined(__x86_64__) || defined(_M_X64)
#include "target_tables_x86_64.h"
#elif defined(__aarch64__) || defined(_M_ARM64)
#include "target_tables_aarch64.h"
#elif defined(__riscv) && __riscv_xlen == 64
#include "target_tables_riscv64.h"
#endif
#include "target_parsing.h"

#include <stdio.h>
#include <string.h>

static void print_hw_features(const FeatureBits *bits) {
    int first = 1;
    for (unsigned i = 0; i < num_features; i++) {
        if (feature_test(bits, feature_table[i].bit)) {
            if (!first) printf(", ");
            printf("%s", feature_table[i].name);
            first = 0;
        }
    }
    printf(" (%u total)\n", feature_popcount(bits));
}

static void test_parse(const char *target_str) {
    printf("\n--- Parsing: \"%s\" ---\n", target_str);
    ParsedTarget parsed[MAX_TARGETS];
    int n = tp_parse_target_string(target_str, parsed, MAX_TARGETS);
    printf("  %d target(s) parsed\n", n);

    for (int i = 0; i < n; i++) {
        printf("  [%d] cpu=%s", i, parsed[i].cpu_name);
        if (parsed[i].flags) printf(" flags=0x%x", parsed[i].flags);
        if (parsed[i].base >= 0) printf(" base=%d", parsed[i].base);
        if (parsed[i].num_extra_features > 0) {
            printf(" features={");
            for (unsigned j = 0; j < parsed[i].num_extra_features; j++) {
                if (j) printf(",");
                printf("%s", parsed[i].extra_features[j]);
            }
            printf("}");
        }
        printf("\n");
    }

    // Resolve
    ResolvedTarget resolved[MAX_TARGETS];
    tp_resolve_targets(parsed, n, resolved, NULL, NULL);

    for (int i = 0; i < n; i++) {
        printf("  Target %d: cpu=%s base=%d flags=0x%x\n",
               i, resolved[i].cpu_name, resolved[i].base, resolved[i].flags);
        printf("    features: ");
        print_hw_features(&resolved[i].features);
        if (resolved[i].ext_features[0])
            printf("    ext: %s\n", resolved[i].ext_features);
    }

    // Get specs
    TargetSpec specs[MAX_TARGETS];
    tp_get_target_specs(resolved, n, specs);
    for (int i = 0; i < n; i++) {
        printf("  Spec %d: cpu=%s features=\"%s\"\n",
               i, specs[i].cpu_name, specs[i].cpu_features);
    }
}

int main() {
    printf("=== Standalone Target Parsing Library Test ===\n");
    printf("No LLVM runtime dependency!\n");
    printf("Database: %u features, %u CPUs\n\n", num_features, num_cpus);

    // Host detection
    printf("Host CPU: %s\n", tp_get_host_cpu_name());
    FeatureBits host_features;
    tp_get_host_features(&host_features);
    printf("Host features: ");
    print_hw_features(&host_features);

    // Check a few specific features
    const FeatureEntry *avx2 = find_feature("avx2");
    const FeatureEntry *avx512f = find_feature("avx512f");
    const FeatureEntry *sse42 = find_feature("sse4.2");
    printf("\nHost has AVX2: %s\n", avx2 && feature_test(&host_features, avx2->bit) ? "yes" : "no");
    printf("Host has AVX512F: %s\n", avx512f && feature_test(&host_features, avx512f->bit) ? "yes" : "no");
    printf("Host has SSE4.2: %s\n", sse42 && feature_test(&host_features, sse42->bit) ? "yes" : "no");

    // CPU lookups
    printf("\n--- CPU lookup test ---\n");
    const char *test_cpus[] = {"generic", "haswell", "skylake", "znver4", "znver3", "nonexistent"};
    for (const char *cpu : test_cpus) {
        const CPUEntry *entry = find_cpu(cpu);
        if (entry) {
            printf("  %s: %u features\n", cpu, feature_popcount(&entry->features));
        } else {
            printf("  %s: NOT FOUND\n", cpu);
        }
    }

    // Feature implications
    printf("\n--- Feature implications ---\n");
    if (avx512f) {
        printf("avx512f implies: ");
        int first = 1;
        for (unsigned i = 0; i < num_features; i++) {
            if (feature_test(&avx512f->implies, feature_table[i].bit)) {
                if (!first) printf(", ");
                printf("%s", feature_table[i].name);
                first = 0;
            }
        }
        printf("\n");
    }

    // Target string parsing
    test_parse("native");
    test_parse("haswell");
    test_parse("generic;haswell;skylake-avx512");
    test_parse("haswell,clone_all;skylake,+avx512f,+avx512bw,-sse4a,opt_size");
    test_parse("znver3;znver4,base(0)");

    // Sysimage matching
    printf("\n--- Sysimage matching ---\n");
    {
        ParsedTarget parsed[MAX_TARGETS];
        ResolvedTarget resolved[MAX_TARGETS];
        int n = tp_parse_target_string("generic;haswell;skylake-avx512", parsed, MAX_TARGETS);
        tp_resolve_targets(parsed, n, resolved, NULL, NULL);

        const char *host_cpu = tp_get_host_cpu_name();
        int match = tp_match_sysimg_target(resolved, n, &host_features, host_cpu);
        printf("  Host: %s\n", host_cpu);
        printf("  Best match: target %d (%s)\n", match,
               match >= 0 ? resolved[match].cpu_name : "none");
    }

    // Clone flags
    printf("\n--- Clone flags ---\n");
    {
        ParsedTarget parsed[MAX_TARGETS];
        ResolvedTarget resolved[MAX_TARGETS];
        int n = tp_parse_target_string("generic;haswell;skylake-avx512", parsed, MAX_TARGETS);
        tp_resolve_targets(parsed, n, resolved, NULL, NULL);
        tp_compute_clone_flags(resolved, n);

        for (int i = 0; i < n; i++) {
            printf("  Target %d (%s): flags=", i, resolved[i].cpu_name);
            uint32_t f = resolved[i].flags;
            if (f & TF_CLONE_ALL)  printf("CLONE_ALL ");
            if (f & TF_CLONE_MATH) printf("CLONE_MATH ");
            if (f & TF_CLONE_LOOP) printf("CLONE_LOOP ");
            if (f & TF_CLONE_SIMD) printf("CLONE_SIMD ");
            if (f & TF_CLONE_CPU)  printf("CLONE_CPU ");
            if (f & TF_CLONE_FLOAT16)  printf("CLONE_FLOAT16 ");
            if (f & TF_CLONE_BFLOAT16) printf("CLONE_BFLOAT16 ");
            if (f == 0) printf("(none)");
            printf("\n");
        }
    }

    printf("\nDone.\n");
    return 0;
}
