/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2017 Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "rd.h"
#include "rddl.h"

#if WITH_LIBDL
#include <dlfcn.h>

#elif defined(_WIN32)

#else
#error "Dynamic library loading not supported on this platform"
#endif



/**
 * @brief Latest thread-local dl error, normalized to suit our logging.
 * @returns a newly allocated string that must be freed
 */
static char *rd_dl_error(void) {
#if WITH_LIBDL
        char *errstr;
        char *s;
        errstr = dlerror();
        if (!errstr)
                return rd_strdup("No error returned from dlerror()");

        errstr = rd_strdup(errstr);
        /* Change newlines to separators. */
        while ((s = strchr(errstr, '\n')))
                *s = '.';

        return errstr;

#elif defined(_WIN32)
        char buf[1024];
        rd_strerror_w32(GetLastError(), buf, sizeof(buf));
        return rd_strdup(buf);
#endif
}

/**
 * @brief Attempt to load library \p path.
 * @returns the library handle (platform dependent, thus opaque) on success,
 *          else NULL.
 */
static rd_dl_hnd_t *
rd_dl_open0(const char *path, char *errstr, size_t errstr_size) {
        void *handle;
        const char *loadfunc;
#if WITH_LIBDL
        loadfunc = "dlopen()";
        handle   = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#elif defined(_WIN32)
        loadfunc = "LoadLibrary()";
        handle   = (void *)LoadLibraryA(path);
#endif
        if (!handle) {
                char *dlerrstr = rd_dl_error();
                rd_snprintf(errstr, errstr_size, "%s failed: %s", loadfunc,
                            dlerrstr);
                rd_free(dlerrstr);
        }
        return (rd_dl_hnd_t *)handle;
}


/**
 * @brief Attempt to load library \p path, possibly with a filename extension
 *        which will be automatically resolved depending on platform.
 * @returns the library handle (platform dependent, thus opaque) on success,
 *          else NULL.
 */
rd_dl_hnd_t *rd_dl_open(const char *path, char *errstr, size_t errstr_size) {
        rd_dl_hnd_t *handle;
        char *extpath;
        size_t pathlen;
        const char *td, *fname;
        const char *solib_ext = SOLIB_EXT;

        /* Try original path first. */
        handle = rd_dl_open0(path, errstr, errstr_size);
        if (handle)
                return handle;

        /* Original path not found, see if we can append the solib_ext
         * filename extension. */

        /* Get filename and filename extension.
         * We can't rely on basename(3) since it is not portable */
        fname = strrchr(path, '/');
#ifdef _WIN32
        td = strrchr(path, '\\');
        if (td > fname)
                fname = td;
#endif
        if (!fname)
                fname = path;

        td = strrchr(fname, '.');

        /* If there is a filename extension ('.' within the last characters)
         * then bail out, we will not append an extension in this case. */
        if (td && td >= fname + strlen(fname) - strlen(SOLIB_EXT))
                return NULL;

        /* Append platform-specific library extension. */
        pathlen = strlen(path);
        extpath = rd_alloca(pathlen + strlen(solib_ext) + 1);
        memcpy(extpath, path, pathlen);
        memcpy(extpath + pathlen, solib_ext, strlen(solib_ext) + 1);

        /* Try again with extension */
        return rd_dl_open0(extpath, errstr, errstr_size);
}


/**
 * @brief Close handle previously returned by rd_dl_open()
 * @remark errors are ignored (what can we do anyway?)
 */
void rd_dl_close(rd_dl_hnd_t *handle) {
#if WITH_LIBDL
        dlclose((void *)handle);
#elif defined(_WIN32)
        FreeLibrary((HMODULE)handle);
#endif
}

/**
 * @brief look up address of \p symbol in library handle \p handle
 * @returns the function pointer on success or NULL on error.
 */
void *rd_dl_sym(rd_dl_hnd_t *handle,
                const char *symbol,
                char *errstr,
                size_t errstr_size) {
        void *func;
#if WITH_LIBDL
        func = dlsym((void *)handle, symbol);
#elif defined(_WIN32)
        func = GetProcAddress((HMODULE)handle, symbol);
#endif
        if (!func) {
                char *dlerrstr = rd_dl_error();
                rd_snprintf(errstr, errstr_size,
                            "Failed to load symbol \"%s\": %s", symbol,
                            dlerrstr);
                rd_free(dlerrstr);
        }
        return func;
}
