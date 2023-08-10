/*
 * Copyright 2021-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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

#include "mc-check-conversions-private.h"
#include "mongocrypt-private.h" // CLIENT_ERR
#include "mongocrypt-util-private.h"

#include "mlib/thread.h"

#include <errno.h>
#include <math.h> // isinf, isnan, isfinite

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

bool size_to_uint32(size_t in, uint32_t *out) {
    BSON_ASSERT_PARAM(out);

    if (in > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)in;
    return true;
}

current_module_result current_module_path(void) {
    mstr ret_str = MSTR_NULL;
    int ret_error = 0;
#ifdef _WIN32
    DWORD acc_size = 512;
    while (!ret_str.data && !ret_error) {
        // Loop until we allocate a large enough buffer or get an error
        wchar_t *path = calloc(acc_size + 1, sizeof(wchar_t));
        SetLastError(0);
        GetModuleFileNameW(NULL, path, acc_size);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            // Try again with more buffer
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
#elif defined(_GNU_SOURCE) || defined(_DARWIN_C_SOURCE) || defined(__FreeBSD__)
    // Darwin/BSD/glibc define extensions for finding dynamic library info from
    // the address of a symbol.
    Dl_info info;
    int rc = dladdr((const void *)current_module_path, &info);
    if (rc == 0) {
        // Failed to resolve the symbol
        ret_error = ENOENT;
    } else {
        ret_str = mstr_copy_cstr(info.dli_fname);
    }
#else
#error "Don't know how to get the module path on this platform"
#endif
    return (current_module_result){.path = ret_str, .error = ret_error};
}

const char *mc_bson_type_to_string(bson_type_t t) {
    switch (t) {
    case BSON_TYPE_EOD: return "EOD";
    case BSON_TYPE_DOUBLE: return "DOUBLE";
    case BSON_TYPE_UTF8: return "UTF8";
    case BSON_TYPE_DOCUMENT: return "DOCUMENT";
    case BSON_TYPE_ARRAY: return "ARRAY";
    case BSON_TYPE_BINARY: return "BINARY";
    case BSON_TYPE_UNDEFINED: return "UNDEFINED";
    case BSON_TYPE_OID: return "OID";
    case BSON_TYPE_BOOL: return "BOOL";
    case BSON_TYPE_DATE_TIME: return "DATE_TIME";
    case BSON_TYPE_NULL: return "NULL";
    case BSON_TYPE_REGEX: return "REGEX";
    case BSON_TYPE_DBPOINTER: return "DBPOINTER";
    case BSON_TYPE_CODE: return "CODE";
    case BSON_TYPE_SYMBOL: return "SYMBOL";
    case BSON_TYPE_CODEWSCOPE: return "CODEWSCOPE";
    case BSON_TYPE_INT32: return "INT32";
    case BSON_TYPE_TIMESTAMP: return "TIMESTAMP";
    case BSON_TYPE_INT64: return "INT64";
    case BSON_TYPE_MAXKEY: return "MAXKEY";
    case BSON_TYPE_MINKEY: return "MINKEY";
    case BSON_TYPE_DECIMAL128: return "DECIMAL128";
    default: return "Unknown";
    }
}

bool mc_iter_document_as_bson(const bson_iter_t *iter, bson_t *bson, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iter);
    BSON_ASSERT_PARAM(bson);
    BSON_ASSERT(status || true);

    uint32_t len;
    const uint8_t *data;

    if (!BSON_ITER_HOLDS_DOCUMENT(iter)) {
        CLIENT_ERR("expected BSON document for field: %s", bson_iter_key(iter));
        return false;
    }

    bson_iter_document(iter, &len, &data);
    if (!bson_init_static(bson, data, len)) {
        CLIENT_ERR("unable to initialize BSON document from field: %s", bson_iter_key(iter));
        return false;
    }

    return true;
}

/* Avoid a conversion warning on glibc for isnan, isinf, and isfinite. Refer:
 * MONGOCRYPT-501. */
MC_BEGIN_CONVERSION_IGNORE

bool mc_isnan(double d) {
    return isnan(d);
}

bool mc_isinf(double d) {
    return isinf(d);
}

bool mc_isfinite(double d) {
    return isfinite(d);
}

MC_END_CONVERSION_IGNORE
