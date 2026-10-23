// check.h — the whole test "framework": CHECK records a failure and keeps going,
// and each test program exits non-zero if anything failed (which is what CTest reads).
#pragma once

#include <cstdio>

inline int gFailures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++gFailures;                                                            \
        }                                                                           \
    } while (0)

// For checks with a message worth printing on failure.
#define CHECK_MSG(cond, ...)                                                        \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, #cond); \
            std::fprintf(stderr, __VA_ARGS__);                                      \
            std::fprintf(stderr, "\n");                                             \
            ++gFailures;                                                            \
        }                                                                           \
    } while (0)

inline int testResult(const char* name) {
    if (gFailures) std::fprintf(stderr, "%s: %d check(s) failed\n", name, gFailures);
    else std::printf("%s: all checks passed\n", name);
    return gFailures ? 1 : 0;
}
