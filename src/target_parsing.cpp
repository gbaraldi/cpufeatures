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
#include "target_internal.h"

#include <cstring>
#include <cstdio>
#include <algorithm>

// ============================================================================
// Internal C++ parsing logic
// ============================================================================

namespace tp {

static std::vector<ParsedTargetInternal> parse_target_string(std::string_view target_str) {
    std::vector<ParsedTargetInternal> result;

    if (target_str.empty()) {
        result.push_back({"native", 0, -1, {}});
        return result;
    }

    auto targets = split(target_str, ';');
    for (auto target_sv : targets) {
        ParsedTargetInternal t;
        auto tokens = split(target_sv, ',');
        if (tokens.empty()) continue;

        // First token is the CPU name
        t.cpu_name = std::string(tokens[0]);

        // Remaining tokens are flags/features
        for (size_t i = 1; i < tokens.size(); i++) {
            auto tok = tokens[i];
            if (tok == "clone_all")       t.flags |= TF_CLONE_ALL;
            else if (tok == "-clone_all") t.flags &= ~TF_CLONE_ALL;
            else if (tok == "opt_size")   t.flags |= TF_OPTSIZE;
            else if (tok == "min_size")   t.flags |= TF_MINSIZE;
            else if (tok.size() > 5 && tok.substr(0, 5) == "base(" && tok.back() == ')') {
                auto num_str = tok.substr(5, tok.size() - 6);
                t.base = std::atoi(std::string(num_str).c_str());
            } else if (!tok.empty() && (tok[0] == '+' || tok[0] == '-')) {
                t.extra_features.emplace_back(tok);
            }
        }

        result.push_back(std::move(t));
    }

    return result;
}

static std::vector<ResolvedTargetInternal> resolve_targets(
        const std::vector<ParsedTargetInternal> &parsed,
        const FeatureBits *host_features,
        const char *host_cpu) {

    std::vector<ResolvedTargetInternal> result;
    result.reserve(parsed.size());

    for (size_t i = 0; i < parsed.size(); i++) {
        ResolvedTargetInternal rt;
        rt.flags = parsed[i].flags;
        rt.base = parsed[i].base >= 0 ? parsed[i].base : (i > 0 ? 0 : -1);

        const auto &name = parsed[i].cpu_name;

        // Resolve CPU name
        if (name == "native" || name.empty()) {
            if (host_cpu && *host_cpu)
                rt.cpu_name = host_cpu;
            else
                rt.cpu_name = tp_get_host_cpu_name();

            if (host_features)
                std::memcpy(&rt.features, host_features, sizeof(FeatureBits));
            else
                tp_get_host_features(&rt.features);
        } else {
            rt.cpu_name = name;
            const CPUEntry *cpu = find_cpu(name.c_str());
            if (cpu) {
                std::memcpy(&rt.features, &cpu->features, sizeof(FeatureBits));
            } else {
                std::fprintf(stderr, "target_parsing: unknown CPU '%s'\n", name.c_str());
                rt.flags |= TF_UNKNOWN_NAME;
                const CPUEntry *gen = find_cpu("generic");
                if (gen) std::memcpy(&rt.features, &gen->features, sizeof(FeatureBits));
            }
        }

        // Apply extra features
        for (const auto &feat : parsed[i].extra_features) {
            bool enable = (feat[0] == '+');
            const char *fname = feat.c_str() + 1;

            const FeatureEntry *fe = find_feature(fname);
            if (fe) {
                if (enable) {
                    feature_set(&rt.features, fe->bit);
                    feature_or(&rt.features, &fe->implies);
                    expand_implied(&rt.features);
                } else {
                    feature_clear(&rt.features, fe->bit);
                    for (unsigned k = 0; k < num_features; k++) {
                        if (feature_test(&feature_table[k].implies, fe->bit))
                            feature_clear(&rt.features, feature_table[k].bit);
                    }
                }
            } else {
                // Unknown feature - pass through to LLVM
                if (!rt.ext_features.empty())
                    rt.ext_features += ',';
                rt.ext_features += feat;
            }
        }

        result.push_back(std::move(rt));
    }

    return result;
}

static std::string build_feature_string(const FeatureBits *features,
                                         const FeatureBits *baseline) {
    std::string result;
    for (unsigned i = 0; i < num_features; i++) {
        int in_feat = feature_test(features, feature_table[i].bit);
        int in_base = baseline ? feature_test(baseline, feature_table[i].bit) : 0;

        if (in_feat && !in_base) {
            if (!result.empty()) result += ',';
            result += '+';
            result += feature_table[i].name;
        } else if (!in_feat && in_base) {
            if (!result.empty()) result += ',';
            result += '-';
            result += feature_table[i].name;
        }
    }
    return result;
}

static std::vector<TargetSpecInternal> get_target_specs(
        const std::vector<ResolvedTargetInternal> &resolved) {
    std::vector<TargetSpecInternal> result;
    result.reserve(resolved.size());

    for (const auto &rt : resolved) {
        TargetSpecInternal spec;
        spec.cpu_name = rt.cpu_name;
        spec.flags = rt.flags;
        spec.base = rt.base;

        const CPUEntry *cpu = find_cpu(rt.cpu_name.c_str());
        const FeatureBits *baseline = cpu ? &cpu->features : nullptr;

        spec.cpu_features = build_feature_string(&rt.features, baseline);

        if (!rt.ext_features.empty()) {
            if (!spec.cpu_features.empty())
                spec.cpu_features += ',';
            spec.cpu_features += rt.ext_features;
        }

        result.push_back(std::move(spec));
    }

    return result;
}

} // namespace tp

// ============================================================================
// extern "C" wrappers — thin shims that copy into caller's C structs
// ============================================================================

int tp_parse_target_string(const char *target_str,
                           ParsedTarget *targets, int max_targets) {
    std::string_view sv(target_str ? target_str : "");
    auto parsed = tp::parse_target_string(sv);

    // Store strings in thread_local storage so .cpu_name / .extra_features
    // pointers remain valid until the next call.
    thread_local std::vector<std::string> name_storage;
    thread_local std::vector<std::string> feat_storage;
    name_storage.clear();
    feat_storage.clear();

    int count = std::min(static_cast<int>(parsed.size()), max_targets);
    for (int i = 0; i < count; i++) {
        std::memset(&targets[i], 0, sizeof(ParsedTarget));

        name_storage.push_back(std::move(parsed[i].cpu_name));
        targets[i].cpu_name = name_storage.back().c_str();
        targets[i].flags = parsed[i].flags;
        targets[i].base = parsed[i].base;

        unsigned nf = std::min(static_cast<unsigned>(parsed[i].extra_features.size()),
                               static_cast<unsigned>(MAX_EXTRA_FEATURES));
        for (unsigned j = 0; j < nf; j++) {
            feat_storage.push_back(std::move(parsed[i].extra_features[j]));
            targets[i].extra_features[j] = feat_storage.back().c_str();
        }
        targets[i].num_extra_features = nf;
    }

    return count;
}

int tp_resolve_targets(const ParsedTarget *parsed, int num_parsed,
                       ResolvedTarget *resolved,
                       const FeatureBits *host_features,
                       const char *host_cpu) {
    // Convert C structs to internal types
    std::vector<ParsedTargetInternal> internal;
    internal.reserve(num_parsed);
    for (int i = 0; i < num_parsed; i++) {
        ParsedTargetInternal p;
        p.cpu_name = parsed[i].cpu_name ? parsed[i].cpu_name : "";
        p.flags = parsed[i].flags;
        p.base = parsed[i].base;
        for (unsigned j = 0; j < parsed[i].num_extra_features; j++) {
            if (parsed[i].extra_features[j])
                p.extra_features.emplace_back(parsed[i].extra_features[j]);
        }
        internal.push_back(std::move(p));
    }

    auto result = tp::resolve_targets(internal, host_features, host_cpu);

    int count = static_cast<int>(result.size());
    for (int i = 0; i < count; i++) {
        std::memset(&resolved[i], 0, sizeof(ResolvedTarget));
        std::strncpy(resolved[i].cpu_name, result[i].cpu_name.c_str(),
                     sizeof(resolved[i].cpu_name) - 1);
        resolved[i].features = result[i].features;
        resolved[i].flags = result[i].flags;
        resolved[i].base = result[i].base;
        std::strncpy(resolved[i].ext_features, result[i].ext_features.c_str(),
                     sizeof(resolved[i].ext_features) - 1);
    }

    return count;
}

void tp_compute_clone_flags(ResolvedTarget *targets, int num_targets) {
    if (num_targets <= 1) return;

    FeatureBits base_features = targets[0].features;

    for (int i = 1; i < num_targets; i++) {
        ResolvedTarget *t = &targets[i];

        FeatureBits diff;
        feature_andnot(&diff, &t->features, &base_features);

        t->flags |= TF_CLONE_CPU | TF_CLONE_LOOP;

#if defined(__x86_64__) || defined(_M_X64)
        if (has_new_feature(diff, "fma") || has_new_feature(diff, "fma4"))
            t->flags |= TF_CLONE_MATH;

        if (has_new_feature(diff, "avx") || has_new_feature(diff, "avx2") ||
            has_new_feature(diff, "avx512f") || has_new_feature(diff, "sse4.1"))
            t->flags |= TF_CLONE_SIMD;

        if (has_new_feature(diff, "avx512fp16"))
            t->flags |= TF_CLONE_FLOAT16;
        if (has_new_feature(diff, "avx512bf16"))
            t->flags |= TF_CLONE_BFLOAT16;
#elif defined(__aarch64__) || defined(_M_ARM64)
        if (has_new_feature(diff, "sve") || has_new_feature(diff, "sve2"))
            t->flags |= TF_CLONE_SIMD;

        if (has_new_feature(diff, "fullfp16"))
            t->flags |= TF_CLONE_FLOAT16;
        if (has_new_feature(diff, "bf16"))
            t->flags |= TF_CLONE_BFLOAT16;
#elif defined(__riscv)
        if (has_new_feature(diff, "v") || has_new_feature(diff, "zve32x") ||
            has_new_feature(diff, "zve64d"))
            t->flags |= TF_CLONE_SIMD;

        if (has_new_feature(diff, "zfh"))
            t->flags |= TF_CLONE_FLOAT16;
        if (has_new_feature(diff, "zvfbfmin"))
            t->flags |= TF_CLONE_BFLOAT16;
#endif
    }
}

// ============================================================================
// Vector register size detection (for dispatch tie-breaking)
// ============================================================================

static unsigned get_vector_reg_size(const FeatureBits *bits) {
#if defined(__x86_64__) || defined(_M_X64)
    if (has_new_feature(*bits, "avx512f")) {
        if (!find_feature("evex512") || has_new_feature(*bits, "evex512"))
            return 512;
        return 256;
    }
    if (has_new_feature(*bits, "avx"))
        return 256;
    return 128;
#elif defined(__aarch64__) || defined(_M_ARM64)
    if (has_new_feature(*bits, "sve"))
        return 2048;
    return 128;
#elif defined(__riscv)
    if (has_new_feature(*bits, "v") || has_new_feature(*bits, "zve64d"))
        return 1024;
    if (has_new_feature(*bits, "zve32x"))
        return 256;
    return 0;
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
        FeatureBits target_hw, host_hw, missing;
        feature_and_out(&target_hw, &targets[i].features, &hw_feature_mask);
        feature_and_out(&host_hw, host_features, &hw_feature_mask);
        feature_andnot(&missing, &target_hw, &host_hw);
        if (feature_any(&missing))
            continue;

        int name_match = (host_cpu && std::strcmp(targets[i].cpu_name, host_cpu) == 0);
        unsigned vec_size = get_vector_reg_size(&targets[i].features);
        FeatureBits hw_feats;
        feature_and_out(&hw_feats, &targets[i].features, &hw_feature_mask);
        unsigned feat_count = feature_popcount(&hw_feats);

        if (name_match > best_name_match) {
            best_idx = i;
            best_name_match = name_match;
            best_vec_size = vec_size;
            best_feat_count = feat_count;
            continue;
        }

        if (best_name_match && !name_match)
            continue;

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
// Feature string generation (C API wrapper)
// ============================================================================

void tp_build_feature_string(const FeatureBits *features,
                             const FeatureBits *baseline,
                             char *buf, size_t bufsize) {
    auto s = tp::build_feature_string(features, baseline);
    std::strncpy(buf, s.c_str(), bufsize - 1);
    buf[bufsize - 1] = '\0';
}

int tp_get_target_specs(const ResolvedTarget *resolved, int num_resolved,
                        TargetSpec *specs) {
    // Convert to internal types
    std::vector<ResolvedTargetInternal> internal;
    internal.reserve(num_resolved);
    for (int i = 0; i < num_resolved; i++) {
        ResolvedTargetInternal rt;
        rt.cpu_name = resolved[i].cpu_name;
        rt.features = resolved[i].features;
        rt.flags = resolved[i].flags;
        rt.base = resolved[i].base;
        rt.ext_features = resolved[i].ext_features;
        internal.push_back(std::move(rt));
    }

    auto result = tp::get_target_specs(internal);

    for (int i = 0; i < num_resolved; i++) {
        std::memset(&specs[i], 0, sizeof(TargetSpec));
        std::strncpy(specs[i].cpu_name, result[i].cpu_name.c_str(),
                     sizeof(specs[i].cpu_name) - 1);
        std::strncpy(specs[i].cpu_features, result[i].cpu_features.c_str(),
                     sizeof(specs[i].cpu_features) - 1);
        specs[i].flags = result[i].flags;
        specs[i].base = result[i].base;
    }

    return num_resolved;
}
