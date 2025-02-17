#pragma once

#if defined(__CUIK__) || !defined(__x86_64__)
#define USE_INTRIN 0
#else
#define USE_INTRIN 1
#endif

#include "cstrings_are_weird.h"
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KILOBYTES(x) ((x) << 10ull)
#define MEGABYTES(x) ((x) << 20ull)
#define GIGABYTES(x) ((x) << 30ull)

#define STR2(x) #x
#define STR(x) STR2(x)

#define _PP_CONCAT__(a, b) a##b
#define _PP_CONCAT(a, b) _PP_CONCAT__(a, b)

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#define Pair(A, B) \
struct {       \
    A _0;      \
    B _1;      \
}

// just because we use a threads fallback layer which can include windows
// and such which is annoying... eventually need to modify that out or something
#ifndef thread_local
#define thread_local _Thread_local
#endif

#define panic(...)       \
do {                     \
    printf(__VA_ARGS__); \
    abort();             \
} while (0)

#define HEAP_ALLOC(s) malloc(s)
#define HEAP_FREE(p) (free(p), (p) = NULL)

#define SWAP(a, b)      \
do {                    \
    typeof(a) temp = a; \
    a = b;              \
    b = temp;           \
} while (0)

void tls_init(void);
void tls_reset(void);
void* tls_push(size_t size);
void* tls_pop(size_t size);
void* tls_save();
void tls_restore(void* p);

void* cuik__valloc(size_t sz);
void cuik__vfree(void* p, size_t sz);

inline static bool cstr_equals(const char* str1, const char* str2) {
    return strcmp(str1, str2) == 0;
}

// returns the number of bytes written
inline static size_t cstr_copy(size_t len, char* dst, const char* src) {
    size_t i = 0;
    while (src[i]) {
        assert(i < len);

        dst[i] = src[i];
        i += 1;
    }
    return i;
}

#ifdef _WIN32
typedef wchar_t* OS_String;
typedef wchar_t OS_Char;

#define OS_STR(x) L##x
#define OS_STR_FMT "S"
#else
typedef char* OS_String;
typedef char OS_Char;

#define OS_STR(x) x
#define OS_STR_FMT "s"

int sprintf_s(char* buffer, size_t len, const char* format, ...);
#endif
