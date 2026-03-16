// CPU name alias resolution.
// Maps alternate CPU names to canonical names in the generated tables.
// From LLVM's ProcessorAlias definitions.
//
// If the primary target doesn't exist in the tables, a fallback is tried.
// This handles cases where tables are generated from a newer LLVM than
// the runtime LLVM (e.g. apple-a18 → apple-m4, but if apple-m4 isn't
// in the tables yet, fall back to apple-a17).

#ifndef CPU_ALIASES_H
#define CPU_ALIASES_H

#include <string_view>

inline const char *resolve_cpu_alias(const char *name) {
    std::string_view sv(name);
    struct Alias { std::string_view from; const char *to; const char *fallback; };
    static constexpr Alias aliases[] = {
        {"apple-m1",  "apple-a14", nullptr},
        {"apple-m2",  "apple-a15", nullptr},
        {"apple-m3",  "apple-a16", nullptr},
        {"apple-a18", "apple-m4",  "apple-a17"},
        {"apple-a19", "apple-m5",  "apple-m4"},
    };
    for (const auto &a : aliases) {
        if (sv == a.from) {
            if (_find_cpu_exact(a.to)) return a.to;
            if (a.fallback && _find_cpu_exact(a.fallback)) return a.fallback;
            return name;
        }
    }
    return name;
}

#endif // CPU_ALIASES_H
