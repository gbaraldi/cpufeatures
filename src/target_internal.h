// Internal C++ types and utilities for the target parsing library.
// NOT part of the public API — used only within src/*.cpp files.

#ifndef TARGET_INTERNAL_H
#define TARGET_INTERNAL_H

#include <string>
#include <string_view>
#include <vector>

// ============================================================================
// Internal C++ types (mirror the C structs but use std::string/vector)
// ============================================================================

struct ParsedTargetInternal {
    std::string cpu_name;
    uint32_t flags = 0;
    int base = -1;
    std::vector<std::string> extra_features; // "+feat" or "-feat"
};

struct ResolvedTargetInternal {
    std::string cpu_name;
    FeatureBits features{};
    uint32_t flags = 0;
    int base = -1;
    std::string ext_features;
};

struct TargetSpecInternal {
    std::string cpu_name;
    std::string cpu_features;
    uint32_t flags = 0;
    int base = -1;
};

// ============================================================================
// String utilities
// ============================================================================

inline std::string_view trim(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
        sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t'))
        sv.remove_suffix(1);
    return sv;
}

inline std::vector<std::string_view> split(std::string_view sv, char delim) {
    std::vector<std::string_view> result;
    while (!sv.empty()) {
        auto pos = sv.find(delim);
        if (pos == std::string_view::npos) {
            auto piece = trim(sv);
            if (!piece.empty()) result.push_back(piece);
            break;
        }
        auto piece = trim(sv.substr(0, pos));
        if (!piece.empty()) result.push_back(piece);
        sv.remove_prefix(pos + 1);
    }
    return result;
}

// Check if a feature diff contains a specific feature by name
inline bool has_new_feature(const FeatureBits &diff, const char *name) {
    const FeatureEntry *fe = find_feature(name);
    return fe && feature_test(&diff, fe->bit);
}

#endif // TARGET_INTERNAL_H
