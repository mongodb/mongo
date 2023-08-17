// Turn on libc extensions so that we can use dladdr() on Unix-like systems
#if defined(__has_include) && !(defined(_GNU_SOURCE) || defined(_DARWIN_C_SOURCE))
#if __has_include(<features.h>)
// We're using a glibc-compatible library
#define _GNU_SOURCE
#elif __has_include(<Availability.h>)
// We're on Apple/Darwin
#define _DARWIN_C_SOURCE
#endif
#else // No __has_include
#if __GNUC__ < 5
// Best guess on older GCC is that we are using glibc
#define _GNU_SOURCE
#endif
#endif

#include "../mongocrypt-dll-private.h"

#ifndef _WIN32

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <dlfcn.h>

mcr_dll mcr_dll_open(const char *filepath) {
    void *handle = dlopen(filepath, RTLD_LAZY | RTLD_LOCAL);
    if (handle == NULL) {
        // Failed to open. Return NULL and copy the error message
        return (mcr_dll){
            ._native_handle = NULL,
            .error_string = mstr_copy_cstr(dlerror()),
        };
    } else {
        // Okay
        return (mcr_dll){
            ._native_handle = handle,
            .error_string = MSTR_NULL,
        };
    }
}

void mcr_dll_close_handle(mcr_dll dll) {
    if (dll._native_handle) {
        dlclose(dll._native_handle);
    }
}

void *mcr_dll_sym(mcr_dll dll, const char *sym) {
    return dlsym(dll._native_handle, sym);
}

#endif

#ifdef __APPLE__

#include <mach-o/dyld.h>
#include <mach-o/nlist.h>

mcr_dll_path_result mcr_dll_path(mcr_dll dll) {
    // Clear the three low bits of the module handle:
    uintptr_t needle = ((uintptr_t)dll._native_handle & ~UINT64_C(0x3));
    // Iterate each loaded dyld image
    /// NOTE: Not thread safe. Is there a thread-safe way to do this?
    for (uint32_t idx = 0; idx < _dyld_image_count(); ++idx) {
        // Get the filepath:
        /// NOTE: Between here and `dlopen`, `dyld_name` could be invalidated by
        /// a concurrent call to `dlclose()`. Is there a better way?
        const char *dyld_name = _dyld_get_image_name(idx);
        // Try and open it. This will return an equivalent pointer to the original
        // handle to the loaded image since they are deduplicated and reference
        // counted.
        void *try_handle = dlopen(dyld_name, RTLD_LAZY);
        if (!dyld_name) {
            // Ouch: `idx` was invalidated before we called _dyld_get_image_name.
            // This will have caused `dlopen()` to return the default handle:
            assert(try_handle == RTLD_DEFAULT);
            continue;
        }
        // Copy the string before closing, to shrink the chance of `dyld_name`
        // being used-after-freed.
        mstr ret_name = mstr_copy_cstr(dyld_name);
        // Mask off the mode bits:
        uintptr_t cur = (uintptr_t)try_handle & ~UINT64_C(0x3);
        // Close our reference to the image. We only care about the handle value.
        dlclose(try_handle);
        if (needle == cur) {
            // We've found the handle
            return (mcr_dll_path_result){.path = ret_name};
        }
        // Not this name.
        mstr_free(ret_name);
    }
    return (mcr_dll_path_result){.error_string = mstr_copy_cstr("Handle not found in loaded modules")};
}

#elif defined(__linux__) || defined(__FreeBSD__)

#include <link.h>

mcr_dll_path_result mcr_dll_path(mcr_dll dll) {
    struct link_map *map;
    int rc = dlinfo(dll._native_handle, RTLD_DI_LINKMAP, &map);
    if (rc == 0) {
        return (mcr_dll_path_result){.path = mstr_copy_cstr(map->l_name)};
    } else {
        return (mcr_dll_path_result){.error_string = mstr_copy_cstr(dlerror())};
    }
}

#elif defined(_WIN32)

// Handled in os_win/os_dll.c

#else

#error "Don't know how to do mcr_dll_path() on this platform"

#endif
