#include "../mongocrypt-dll-private.h"

#ifdef _WIN32

#include <mlib/error.h>
#include <mlib/path.h>
#include <mlib/str.h>

#include <stdio.h>
#include <string.h>

#include <windows.h>

mcr_dll mcr_dll_open(const char *filepath_) {
    // Convert all slashes to the native Windows separator
    mstr filepath = mpath_to_format(MPATH_WIN32, mstrv_view_cstr(filepath_), MPATH_WIN32);
    // Check if the path is just a filename.
    bool is_just_filename = mstr_eq(mpath_filename(filepath.view, MPATH_WIN32), filepath.view);
    if (!is_just_filename) {
        // If the path is only a filename, we'll allow LoadLibrary() to do a
        // proper full DLL search. If the path is NOT just a filename, resolve the
        // given path to a single unambiguous absolute path to suppress
        // LoadLibrary()'s DLL search behavior.
        mstr_assign(&filepath, mpath_absolute(filepath.view, MPATH_WIN32));
    }
    mstr_widen_result wide = mstr_win32_widen(filepath.view);
    mstr_free(filepath);
    if (wide.error) {
        return (mcr_dll){._native_handle = NULL, .error_string = merror_system_error_string(wide.error)};
    }
    HMODULE lib = LoadLibraryW(wide.wstring);
    if (lib == NULL) {
        return (mcr_dll){._native_handle = NULL, .error_string = merror_system_error_string(GetLastError())};
    }
    free(wide.wstring);
    return (mcr_dll){.error_string = NULL, ._native_handle = lib};
}

void mcr_dll_close_handle(mcr_dll dll) {
    if (dll._native_handle) {
        FreeLibrary(dll._native_handle);
    }
}

void *mcr_dll_sym(mcr_dll dll, const char *sym) {
    return GetProcAddress(dll._native_handle, sym);
}

mcr_dll_path_result mcr_dll_path(mcr_dll dll) {
    mstr ret_str = MSTR_NULL;
    int ret_error = 0;
    DWORD acc_size = 512;
    while (!ret_str.data && !ret_error) {
        // Loop until we allocate a large enough buffer or get an error
        wchar_t *path = calloc((size_t)acc_size + 1u, sizeof(wchar_t));
        SetLastError(0);
        GetModuleFileNameW(dll._native_handle, path, acc_size);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            // Try again with more buffer
            /* DWORD is a 32-bit unsigned integer */
            assert(acc_size <= UINT32_MAX / 2u);
            acc_size *= 2;
        } else if (GetLastError() != 0) {
            ret_error = GetLastError();
        } else {
            mstr_narrow_result narrow = mstr_win32_narrow(path);
            // GetModuleFileNameW should never return invalid Unicode:
            assert(narrow.error == 0);
            ret_str = narrow.string;
        }
        free(path);
    }
    return (mcr_dll_path_result){
        .path = ret_str,
        .error_string = merror_system_error_string(ret_error),
    };
}

#endif
