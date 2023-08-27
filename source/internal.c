//
// Created by jan on 26.8.2023.
//

#include <assert.h>
#include <malloc.h>
#include <stdarg.h>
#include "internal.h"

void* jvm_alloc(const jvm_allocator* alc, uint64_t size)
{
    return alc->allocation_callbacks.allocate(alc->allocation_callbacks.state, size);
}

void* jvm_realloc(const jvm_allocator* alc, void* ptr, uint64_t new_size)
{
    return alc->allocation_callbacks.reallocate(alc->allocation_callbacks.state, ptr, new_size);
}

void jvm_free(const jvm_allocator* alc, void* ptr)
{
    alc->allocation_callbacks.free(alc->allocation_callbacks.state, ptr);
}

static void* default_alloc(void* state, uint64_t size)
{
    assert((void*) 0xCafe == state);
    (void) state;
    return malloc(size);
}

static void* default_realloc(void* state, void* ptr, uint64_t new_size)
{
    assert((void*) 0xCafe == state);
    (void) state;
    return realloc(ptr, new_size);
}

static void default_free(void* state, void* ptr)
{
    assert((void*) 0xCafe == state);
    (void) state;
    free(ptr);
}

const jvm_allocation_callbacks DEFAULT_ALLOC_CALLBACKS =
        {
                .allocate = default_alloc,
                .reallocate = default_realloc,
                .free = default_free,
                .state = (void*) 0xCafe,
        };

static void default_report_function(void* state, const char* msg, const char* file, int line, const char* function)
{
    assert((void*) 0xBadBeef == state);
    (void) state;
    fprintf(stderr, "JVM Error report (%s:%d - %s): \"%s\"\n", file, line, function, msg);
}

const jvm_error_callbacks DEFAULT_ERROR_CALLBACKS =
        {
                .report = default_report_function,
                .state = (void*) 0xBadBeef,
        };

void jvm_report_error(const jvm_allocator* alc, const char* fmt, const char* file, int line, const char* function, ...)
{
    if (!alc->error_callbacks.report)
    {
        return;
    }
    va_list args, cpy;
    va_start(args, function);
    va_copy(cpy, args);
    const int len = vsnprintf(NULL, 0, fmt, cpy);
    va_end(cpy);
    if (len > 0)
    {
        char* const buffer = jvm_alloc(alc, len + 1);
        if (buffer)
        {
            vsnprintf(buffer, len + 1, fmt, args);
            alc->error_callbacks.report(alc->error_callbacks.state, buffer, file, line, function);
            jvm_free(alc, buffer);
        }
    }

    va_end(args);
}
