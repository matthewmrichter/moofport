/* t.h: minimal test harness for MoofPort host tests.
 *
 * Write tests as  static void test_xxx(void) { T_CHECK(...); T_EQ(a, b); }
 * and end the file with  T_MAIN(test_a, test_b, ...)  (at most 32 tests).
 * Run:  ./build/test_ring            all tests in the file
 *       ./build/test_ring wrap       only tests whose name contains "wrap"
 * A failing check prints file:line and returns from the test; the exe exits 1.
 */
#ifndef T_H
#define T_H

#include <stdio.h>
#include <string.h>

static int t_fails;
static int t_cur_failed;

#define T_FAIL_(fmt, ...)                                                    \
    do {                                                                     \
        fprintf(stderr, "  FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,       \
                __VA_ARGS__);                                                \
        t_cur_failed = 1;                                                    \
        return;                                                              \
    } while (0)

#define T_CHECK(c)                                                           \
    do {                                                                     \
        if (!(c))                                                            \
            T_FAIL_("%s", #c);                                               \
    } while (0)

#define T_EQ(a, b)                                                           \
    do {                                                                     \
        long t_a_ = (long)(a), t_b_ = (long)(b);                             \
        if (t_a_ != t_b_)                                                    \
            T_FAIL_("%s == %s (%ld != %ld)", #a, #b, t_a_, t_b_);            \
    } while (0)

#define T_MEM(a, b, n)                                                       \
    do {                                                                     \
        if (memcmp((a), (b), (n)) != 0)                                      \
            T_FAIL_("memcmp(%s, %s, %s)", #a, #b, #n);                       \
    } while (0)

typedef void (*t_fn)(void);

#define T_NAMES_(...) #__VA_ARGS__

#define T_MAIN(...)                                                          \
    int main(int argc, char **argv)                                          \
    {                                                                        \
        t_fn fns[] = { __VA_ARGS__ };                                        \
        static char names[] = T_NAMES_(__VA_ARGS__);                         \
        const char *nm[32];                                                  \
        unsigned n = (unsigned)(sizeof(fns) / sizeof(fns[0])), i, run = 0; \
        char *s = names;                                                     \
        if (n > 32) {                                                        \
            fprintf(stderr, "T_MAIN: at most 32 tests\n");                   \
            return 2;                                                        \
        }                                                                    \
        for (i = 0; i < n; i++) {                                            \
            while (*s == ' ' || *s == ',' || *s == '\n' || *s == '\t')       \
                s++;                                                         \
            nm[i] = s;                                                       \
            while (*s && *s != ',')                                          \
                s++;                                                         \
            if (*s) {                                                        \
                char *e = s;                                                 \
                while (e > nm[i] && (e[-1] == ' ' || e[-1] == '\t'))         \
                    e--;                                                     \
                *e = 0;                                                      \
                s++;                                                         \
            }                                                                \
        }                                                                    \
        for (i = 0; i < n; i++) {                                            \
            if (argc > 1 && !strstr(nm[i], argv[1]))                         \
                continue;                                                    \
            t_cur_failed = 0;                                                \
            fns[i]();                                                        \
            run++;                                                           \
            printf("%s %s\n", t_cur_failed ? "FAIL" : "ok  ", nm[i]);        \
            if (t_cur_failed)                                                \
                t_fails++;                                                   \
        }                                                                    \
        printf("%u run, %d failed\n", run, t_fails);                         \
        return (t_fails || run == 0) ? 1 : 0;                                \
    }

#endif
