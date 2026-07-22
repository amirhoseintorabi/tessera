/* SPDX-License-Identifier: MIT */
#ifndef TESSERA_TESTS_CHECK_H
#define TESSERA_TESTS_CHECK_H

/*
 * The assertion macros, shared by every test in this directory.
 *
 * Deliberately not a framework. Each test binary is a single translation unit
 * plus this header, links only the library it tests, and returns non-zero on
 * failure -- which is all ctest needs, and means the tests build for a target
 * with no filesystem and no argv if anyone wants to run them there.
 *
 * Nothing here stops at the first failure. A run that reports "3 failures" and
 * where they are is more useful than one that reports the first and abandons
 * the rest, particularly when a single wrong constant makes twenty checks fail
 * in a recognisable pattern.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;
static const char *current = "";

static void begin(const char *name)
{
    current = name;
    printf("  %s\n", name);
}

static void fail(const char *file, int line, const char *what)
{
    failures++;
    printf("    FAIL %s:%d in \"%s\"\n           %s\n", file, line, current, what);
}

#define CHECK(expr)                                            \
    do {                                                       \
        checks++;                                              \
        if (!(expr)) { fail(__FILE__, __LINE__, #expr); }      \
    } while (0)

#define CHECK_EQ_I(actual, expected)                                            \
    do {                                                                        \
        checks++;                                                               \
        long a_ = (long) (actual), e_ = (long) (expected);                      \
        if (a_ != e_) {                                                         \
            char m[192];                                                        \
            snprintf(m, sizeof m, "%s == %ld, expected %ld", #actual, a_, e_);  \
            fail(__FILE__, __LINE__, m);                                        \
        }                                                                       \
    } while (0)

/* Status codes compare as integers but print as names, because "TESS_ERR_FULL,
 * expected TESS_ERR_ARG" says what went wrong and "2, expected 1" does not. */
#define CHECK_STATUS(actual, expected)                                          \
    do {                                                                        \
        checks++;                                                               \
        tess_status a_ = (actual), e_ = (expected);                               \
        if (a_ != e_) {                                                         \
            char m[192];                                                        \
            snprintf(m, sizeof m, "%s == %s, expected %s",                      \
                     #actual, tess_status_name(a_), tess_status_name(e_));          \
            fail(__FILE__, __LINE__, m);                                        \
        }                                                                       \
    } while (0)

/* The cast to double happens inside the macro rather than at the call sites:
 * comparing a float against a double literal is a -Wdouble-promotion warning
 * under clang, and fixing that once here beats fixing it sixty times. */
#define CHECK_NEAR(actual, expected, tol)                                        \
    do {                                                                         \
        checks++;                                                                \
        double a_ = (double) (actual), e_ = (double) (expected);                 \
        if (fabs(a_ - e_) > (double) (tol)) {                                    \
            char m[192];                                                         \
            snprintf(m, sizeof m, "%s == %.9f, expected %.9f", #actual, a_, e_); \
            fail(__FILE__, __LINE__, m);                                         \
        }                                                                        \
    } while (0)

#define CHECK_STR_EQ(actual, expected)                                          \
    do {                                                                        \
        checks++;                                                               \
        const char *a_ = (actual), *e_ = (expected);                            \
        if (strcmp(a_, e_) != 0) {                                              \
            char m[256];                                                        \
            snprintf(m, sizeof m, "%s == \"%s\", expected \"%s\"",              \
                     #actual, a_, e_);                                          \
            fail(__FILE__, __LINE__, m);                                        \
        }                                                                       \
    } while (0)

#define REPORT(suite)                                            \
    do {                                                         \
        printf("\n%s: %d checks, %d failure(s)\n",                \
               (suite), checks, failures);                        \
        return failures ? 1 : 0;                                  \
    } while (0)

#endif /* TESSERA_TESTS_CHECK_H */
