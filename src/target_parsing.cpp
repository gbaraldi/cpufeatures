// Standalone target parsing library implementation.
// No LLVM runtime dependency - uses pre-generated tables.

// Include generated tables FIRST (defines FeatureBits etc.)
#if defined(__x86_64__) || defined(_M_X64)
#include "target_tables_x86_64.h"
#elif defined(__aarch64__) || defined(_M_ARM64)
#include "target_tables_aarch64.h"
#elif defined(__riscv) && __riscv_xlen == 64
#include "target_tables_riscv64.h"
#else
#error "Unsupported architecture - generate tables with gen_target_tables"
#endif

#include "target_parsing.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

// ============================================================================
// Target string parsing
// ============================================================================

int tp_parse_target_string(const char *target_str,
                           ParsedTarget *targets, int max_targets) {
    if (!target_str || !*target_str) {
        // Default: single "native" target
        if (max_targets < 1) return 0;
        memset(&targets[0], 0, sizeof(ParsedTarget));
        targets[0].cpu_name = "native";
        targets[0].base = -1;
        return 1;
    }

    int count = 0;
    const char *p = target_str;

    while (*p && count < max_targets) {
        ParsedTarget *t = &targets[count];
        memset(t, 0, sizeof(ParsedTarget));
        t->base = -1;

        // Skip whitespace
        while (*p && isspace(*p)) p++;
        if (!*p) break;

        // Find end of this target (semicolon or end)
        const char *end = strchr(p, ';');
        if (!end) end = p + strlen(p);

        // First token before comma is the CPU name
        const char *comma = p;
        while (comma < end && *comma != ',') comma++;

        // Extract CPU name (trim whitespace)
        while (p < comma && isspace(*p)) p++;
        const char *name_end = comma;
        while (name_end > p && isspace(*(name_end - 1))) name_end--;

        // Store CPU name - we need a stable pointer, use the input string
        // (it's expected to remain valid)
        // We'll store it as a pointer into the original string,
        // but we need null-termination... Use a static buffer approach
        static char cpu_names[MAX_TARGETS][128];
        size_t len = name_end - p;
        if (len >= sizeof(cpu_names[0])) len = sizeof(cpu_names[0]) - 1;
        memcpy(cpu_names[count], p, len);
        cpu_names[count][len] = '\0';
        t->cpu_name = cpu_names[count];

        // Process features/flags after the CPU name
        p = comma;
        while (p < end) {
            if (*p == ',') p++;
            while (p < end && isspace(*p)) p++;
            if (p >= end) break;

            // Find end of this token
            const char *tok_end = p;
            while (tok_end < end && *tok_end != ',') tok_end++;
            while (tok_end > p && isspace(*(tok_end - 1))) tok_end--;

            size_t tok_len = tok_end - p;
            if (tok_len == 0) { p = tok_end; continue; }

            // Check special flags
            if (tok_len == 9 && strncmp(p, "clone_all", 9) == 0) {
                t->flags |= TF_CLONE_ALL;
            } else if (tok_len == 10 && strncmp(p, "-clone_all", 10) == 0) {
                t->flags &= ~TF_CLONE_ALL;
            } else if (tok_len == 8 && strncmp(p, "opt_size", 8) == 0) {
                t->flags |= TF_OPTSIZE;
            } else if (tok_len == 8 && strncmp(p, "min_size", 8) == 0) {
                t->flags |= TF_MINSIZE;
            } else if (tok_len > 5 && strncmp(p, "base(", 5) == 0 && p[tok_len-1] == ')') {
                // base(N)
                char num_buf[16];
                size_t num_len = tok_len - 6;
                if (num_len < sizeof(num_buf)) {
                    memcpy(num_buf, p + 5, num_len);
                    num_buf[num_len] = '\0';
                    t->base = atoi(num_buf);
                }
            } else if ((*p == '+' || *p == '-') && t->num_extra_features < MAX_EXTRA_FEATURES) {
                // Feature flag: store pointer into target string
                // We need null-terminated copies
                static char feat_bufs[MAX_TARGETS * MAX_EXTRA_FEATURES][64];
                int fi = count * MAX_EXTRA_FEATURES + t->num_extra_features;
                size_t fl = tok_len < 63 ? tok_len : 63;
                memcpy(feat_bufs[fi], p, fl);
                feat_bufs[fi][fl] = '\0';
                t->extra_features[t->num_extra_features++] = feat_bufs[fi];
            }

            p = tok_end;
        }

        count++;
        if (*end == ';') end++;
        p = end;
    }

    return count;
}

// ============================================================================
// Target resolution
// ============================================================================

static void copy_features(FeatureBits *dst, const FeatureBits *src) {
    memcpy(dst, src, sizeof(FeatureBits));
}

int tp_resolve_targets(const ParsedTarget *parsed, int num_parsed,
                       ResolvedTarget *resolved,
                       const FeatureBits *host_features,
                       const char *host_cpu) {
    for (int i = 0; i < num_parsed; i++) {
        ResolvedTarget *rt = &resolved[i];
        memset(rt, 0, sizeof(ResolvedTarget));
        rt->flags = parsed[i].flags;
        rt->base = parsed[i].base >= 0 ? parsed[i].base : (i > 0 ? 0 : -1);

        const char *name = parsed[i].cpu_name;

        // Resolve CPU name
        if (strcmp(name, "native") == 0 || name[0] == '\0') {
            if (host_cpu && *host_cpu) {
                strncpy(rt->cpu_name, host_cpu, sizeof(rt->cpu_name) - 1);
            } else {
                strncpy(rt->cpu_name, tp_get_host_cpu_name(), sizeof(rt->cpu_name) - 1);
            }

            if (host_features) {
                copy_features(&rt->features, host_features);
            } else {
                tp_get_host_features(&rt->features);
            }
        } else {
            strncpy(rt->cpu_name, name, sizeof(rt->cpu_name) - 1);
            const CPUEntry *cpu = find_cpu(name);
            if (cpu) {
                copy_features(&rt->features, &cpu->features);
            } else {
                fprintf(stderr, "target_parsing: unknown CPU '%s'\n", name);
                rt->flags |= TF_UNKNOWN_NAME;
                const CPUEntry *gen = find_cpu("generic");
                if (gen) copy_features(&rt->features, &gen->features);
            }
        }

        // Apply extra features
        for (unsigned j = 0; j < parsed[i].num_extra_features; j++) {
            const char *feat = parsed[i].extra_features[j];
            if (!feat) continue;

            bool enable = (feat[0] == '+');
            const char *fname = feat + 1;

            const FeatureEntry *fe = find_feature(fname);
            if (fe) {
                if (enable) {
                    feature_set(&rt->features, fe->bit);
                    // Expand implications
                    feature_or(&rt->features, &fe->implies);
                    expand_implied(&rt->features);
                } else {
                    feature_clear(&rt->features, fe->bit);
                    // Clear features that depend on this one
                    for (unsigned k = 0; k < num_features; k++) {
                        if (feature_test(&feature_table[k].implies, fe->bit)) {
                            feature_clear(&rt->features, feature_table[k].bit);
                        }
                    }
                }
            } else {
                // Unknown feature - pass through to LLVM
                size_t cur_len = strlen(rt->ext_features);
                if (cur_len > 0 && cur_len < sizeof(rt->ext_features) - 2) {
                    rt->ext_features[cur_len] = ',';
                    cur_len++;
                }
                size_t feat_len = strlen(feat);
                if (cur_len + feat_len < sizeof(rt->ext_features) - 1) {
                    memcpy(rt->ext_features + cur_len, feat, feat_len + 1);
                }
            }
        }
    }

    return num_parsed;
}

// ============================================================================
// Clone flag computation
// ============================================================================

void tp_compute_clone_flags(ResolvedTarget *targets, int num_targets) {
    if (num_targets <= 1) return;

    FeatureBits base_features = targets[0].features;

    for (int i = 1; i < num_targets; i++) {
        ResolvedTarget *t = &targets[i];

        // Compute diff: features in this target but not in base
        FeatureBits diff;
        feature_andnot(&diff, &t->features, &base_features);

        t->flags |= TF_CLONE_CPU | TF_CLONE_LOOP;

#if defined(__x86_64__) || defined(_M_X64)
        // Check for new scalar math features (FMA)
        const FeatureEntry *fma = find_feature("fma");
        const FeatureEntry *fma4 = find_feature("fma4");
        if ((fma && feature_test(&diff, fma->bit)) ||
            (fma4 && feature_test(&diff, fma4->bit))) {
            t->flags |= TF_CLONE_MATH;
        }

        // Check for new SIMD features
        const FeatureEntry *avx = find_feature("avx");
        const FeatureEntry *avx2 = find_feature("avx2");
        const FeatureEntry *avx512f = find_feature("avx512f");
        const FeatureEntry *sse41 = find_feature("sse4.1");
        if ((avx && feature_test(&diff, avx->bit)) ||
            (avx2 && feature_test(&diff, avx2->bit)) ||
            (avx512f && feature_test(&diff, avx512f->bit)) ||
            (sse41 && feature_test(&diff, sse41->bit))) {
            t->flags |= TF_CLONE_SIMD;
        }

        // FP16/BF16
        const FeatureEntry *avx512fp16 = find_feature("avx512fp16");
        const FeatureEntry *avx512bf16 = find_feature("avx512bf16");
        if (avx512fp16 && feature_test(&diff, avx512fp16->bit))
            t->flags |= TF_CLONE_FLOAT16;
        if (avx512bf16 && feature_test(&diff, avx512bf16->bit))
            t->flags |= TF_CLONE_BFLOAT16;
#elif defined(__aarch64__) || defined(_M_ARM64)
        // SVE
        const FeatureEntry *sve = find_feature("sve");
        const FeatureEntry *sve2 = find_feature("sve2");
        if ((sve && feature_test(&diff, sve->bit)) ||
            (sve2 && feature_test(&diff, sve2->bit))) {
            t->flags |= TF_CLONE_SIMD;
        }

        const FeatureEntry *fullfp16 = find_feature("fullfp16");
        const FeatureEntry *bf16 = find_feature("bf16");
        if (fullfp16 && feature_test(&diff, fullfp16->bit))
            t->flags |= TF_CLONE_FLOAT16;
        if (bf16 && feature_test(&diff, bf16->bit))
            t->flags |= TF_CLONE_BFLOAT16;
#elif defined(__riscv)
        // RISC-V V extension
        const FeatureEntry *rv_v = find_feature("v");
        const FeatureEntry *zve32x = find_feature("zve32x");
        const FeatureEntry *zve64d = find_feature("zve64d");
        if ((rv_v && feature_test(&diff, rv_v->bit)) ||
            (zve32x && feature_test(&diff, zve32x->bit)) ||
            (zve64d && feature_test(&diff, zve64d->bit))) {
            t->flags |= TF_CLONE_SIMD;
        }

        const FeatureEntry *zfh = find_feature("zfh");
        const FeatureEntry *zvfbfmin = find_feature("zvfbfmin");
        if (zfh && feature_test(&diff, zfh->bit))
            t->flags |= TF_CLONE_FLOAT16;
        if (zvfbfmin && feature_test(&diff, zvfbfmin->bit))
            t->flags |= TF_CLONE_BFLOAT16;
#endif
    }
}

// ============================================================================
// Vector register size detection (for dispatch tie-breaking)
// ============================================================================

static unsigned get_vector_reg_size(const FeatureBits *bits) {
#if defined(__x86_64__) || defined(_M_X64)
    const FeatureEntry *avx512f = find_feature("avx512f");
    const FeatureEntry *evex512 = find_feature("evex512");
    const FeatureEntry *avx = find_feature("avx");

    if (avx512f && feature_test(bits, avx512f->bit)) {
        if (!evex512 || feature_test(bits, evex512->bit))
            return 512;
        return 256;
    }
    if (avx && feature_test(bits, avx->bit))
        return 256;
    return 128;
#elif defined(__aarch64__) || defined(_M_ARM64)
    const FeatureEntry *sve = find_feature("sve");
    if (sve && feature_test(bits, sve->bit))
        return 2048;
    return 128;
#elif defined(__riscv)
    const FeatureEntry *rv_v = find_feature("v");
    const FeatureEntry *zve64d = find_feature("zve64d");
    const FeatureEntry *zve32x = find_feature("zve32x");
    if ((rv_v && feature_test(bits, rv_v->bit)) ||
        (zve64d && feature_test(bits, zve64d->bit)))
        return 1024; // RVV is scalable, treat as "wider than fixed"
    if (zve32x && feature_test(bits, zve32x->bit))
        return 256;
    return 0; // No vector
#else
    return 128;
#endif
}

// ============================================================================
// Sysimage target matching
// ============================================================================

int tp_match_sysimg_target(const ResolvedTarget *targets, int num_targets,
                           const FeatureBits *host_features,
                           const char *host_cpu) {
    if (num_targets == 0) return -1;

    int best_idx = 0;
    int best_name_match = 0;
    unsigned best_vec_size = 0;
    unsigned best_feat_count = 0;

    for (int i = 0; i < num_targets; i++) {
        // Only compare hardware features for compatibility
        // (tuning features differ between vendors and shouldn't block matching)
        FeatureBits target_hw, host_hw, missing;
        feature_and_out(&target_hw, &targets[i].features, &hw_feature_mask);
        feature_and_out(&host_hw, host_features, &hw_feature_mask);
        feature_andnot(&missing, &target_hw, &host_hw);
        if (feature_any(&missing))
            continue;

        int name_match = (host_cpu && strcmp(targets[i].cpu_name, host_cpu) == 0);
        unsigned vec_size = get_vector_reg_size(&targets[i].features);
        // Count only hardware features for ranking
        FeatureBits hw_feats;
        feature_and_out(&hw_feats, &targets[i].features, &hw_feature_mask);
        unsigned feat_count = feature_popcount(&hw_feats);

        // If we found a name match and previous best wasn't, prefer this
        if (name_match > best_name_match) {
            best_idx = i;
            best_name_match = name_match;
            best_vec_size = vec_size;
            best_feat_count = feat_count;
            continue;
        }

        // Don't downgrade from name match
        if (best_name_match && !name_match)
            continue;

        // Prefer larger vector registers
        if (vec_size > best_vec_size ||
            (vec_size == best_vec_size && feat_count > best_feat_count) ||
            (vec_size == best_vec_size && feat_count == best_feat_count && i > best_idx)) {
            best_idx = i;
            best_vec_size = vec_size;
            best_feat_count = feat_count;
        }
    }

    return best_idx;
}

// ============================================================================
// Feature string generation
// ============================================================================

void tp_build_feature_string(const FeatureBits *features,
                             const FeatureBits *baseline,
                             char *buf, size_t bufsize) {
    buf[0] = '\0';
    size_t pos = 0;

    for (unsigned i = 0; i < num_features; i++) {
        int in_feat = feature_test(features, feature_table[i].bit);
        int in_base = baseline ? feature_test(baseline, feature_table[i].bit) : 0;

        const char *prefix = NULL;
        if (in_feat && !in_base) prefix = "+";
        else if (!in_feat && in_base) prefix = "-";
        else continue;

        size_t name_len = strlen(feature_table[i].name);
        size_t needed = (pos > 0 ? 1 : 0) + 1 + name_len; // comma + +/- + name
        if (pos + needed >= bufsize - 1) break;

        if (pos > 0) buf[pos++] = ',';
        buf[pos++] = prefix[0];
        memcpy(buf + pos, feature_table[i].name, name_len);
        pos += name_len;
    }
    buf[pos] = '\0';
}

int tp_get_target_specs(const ResolvedTarget *resolved, int num_resolved,
                        TargetSpec *specs) {
    for (int i = 0; i < num_resolved; i++) {
        memset(&specs[i], 0, sizeof(TargetSpec));
        strncpy(specs[i].cpu_name, resolved[i].cpu_name, sizeof(specs[i].cpu_name) - 1);
        specs[i].flags = resolved[i].flags;
        specs[i].base = resolved[i].base;

        // Get the default features for this CPU
        const CPUEntry *cpu = find_cpu(resolved[i].cpu_name);
        const FeatureBits *baseline = cpu ? &cpu->features : NULL;

        tp_build_feature_string(&resolved[i].features, baseline,
                                specs[i].cpu_features, sizeof(specs[i].cpu_features));

        // Append ext_features
        if (resolved[i].ext_features[0]) {
            size_t cur = strlen(specs[i].cpu_features);
            size_t ext = strlen(resolved[i].ext_features);
            if (cur + ext + 2 < sizeof(specs[i].cpu_features)) {
                if (cur > 0) specs[i].cpu_features[cur++] = ',';
                memcpy(specs[i].cpu_features + cur, resolved[i].ext_features, ext + 1);
            }
        }
    }
    return num_resolved;
}
