/*
 * Copyright (c) 2017-2020, 2023 [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1.  Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 * 2.  Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#include <string.h>
#else
#include "uniwin.h"
#endif
#include <sys/stat.h>
#include <stdarg.h>
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#include <rnp/rnp_def.h>
#include "rnp.h"
#include "stream-common.h"
#include "stream-packet.h"
#include "types.h"
#include "file-utils.h"
#include "crypto/mem.h"
#include <algorithm>
#include <memory>

bool
pgp_source_t::read(void *buf, size_t len, size_t *readres)
{
    size_t left = len;
    size_t read;
    bool   readahead = cache ? cache->readahead : false;

    if (error_) {
        return false;
    }

    if (eof_ || (len == 0)) {
        *readres = 0;
        return true;
    }

    // Do not read more then available if source size is known
    if (knownsize && (readb + len > size)) {
        len = size - readb;
        left = len;
        readahead = false;
    }

    // Check whether we have cache and there is data inside
    if (cache && (cache->len > cache->pos)) {
        read = cache->len - cache->pos;
        if (read >= len) {
            memcpy(buf, &cache->buf[cache->pos], len);
            cache->pos += len;
            goto finish;
        } else {
            memcpy(buf, &cache->buf[cache->pos], read);
            cache->pos += read;
            buf = (uint8_t *) buf + read;
            left = len - read;
        }
    }

    // If we got here then we have empty cache or no cache at all
    while (left > 0) {
        if (left > sizeof(cache->buf) || !readahead || !cache) {
            // If there is no cache or chunk is larger then read directly
            if (!raw_read(this, buf, left, &read)) {
                error_ = 1;
                return false;
            }
            if (!read) {
                eof_ = true;
                len = len - left;
                goto finish;
            }
            left -= read;
            buf = (uint8_t *) buf + read;
        } else {
            // Try to fill the cache to avoid small reads
            if (!raw_read(this, &cache->buf[0], sizeof(cache->buf), &read)) {
                error_ = true;
                return false;
            }
            if (!read) {
                eof_ = true;
                len = len - left;
                goto finish;
            } else if (read < left) {
                memcpy(buf, &cache->buf[0], read);
                left -= read;
                buf = (uint8_t *) buf + read;
            } else {
                memcpy(buf, &cache->buf[0], left);
                cache->pos = left;
                cache->len = read;
                goto finish;
            }
        }
    }

finish:
    readb += len;
    if (knownsize && (readb == size)) {
        eof_ = true;
    }
    *readres = len;
    return true;
}

bool
pgp_source_t::read_eq(void *buf, size_t len)
{
    size_t res = 0;
    return read(buf, len, &res) && (res == len);
}

bool
pgp_source_t::peek(void *buf, size_t len, size_t *peeked)
{
    if (error_) {
        return false;
    }
    if (!cache || (len > sizeof(cache->buf))) {
        return false;
    }
    if (eof_) {
        *peeked = 0;
        return true;
    }

    size_t read = 0;
    bool   readahead = cache->readahead;
    // Do not read more then available if source size is known
    if (knownsize && (readb + len > size)) {
        len = size - readb;
        readahead = false;
    }

    if (cache->len - cache->pos >= len) {
        if (buf) {
            memcpy(buf, &cache->buf[cache->pos], len);
        }
        *peeked = len;
        return true;
    }

    if (cache->pos > 0) {
        memmove(&cache->buf[0], &cache->buf[cache->pos], cache->len - cache->pos);
        cache->len -= cache->pos;
        cache->pos = 0;
    }

    while (cache->len < len) {
        read = readahead ? sizeof(cache->buf) - cache->len : len - cache->len;
        if (knownsize && (readb + read > size)) {
            read = size - readb;
        }
        if (!raw_read(this, &cache->buf[cache->len], read, &read)) {
            error_ = true;
            return false;
        }
        if (!read) {
            if (buf) {
                memcpy(buf, &cache->buf[0], cache->len);
            }
            *peeked = cache->len;
            return true;
        }
        cache->len += read;
        if (cache->len >= len) {
            if (buf) {
                memcpy(buf, cache->buf, len);
            }
            *peeked = len;
            return true;
        }
    }
    return false;
}

bool
pgp_source_t::peek_eq(void *buf, size_t len)
{
    size_t res = 0;
    return peek(buf, len, &res) && (res == len);
}

void
pgp_source_t::skip(size_t len)
{
    if (cache && (cache->len - cache->pos >= len)) {
        readb += len;
        cache->pos += len;
        return;
    }

    size_t  res = 0;
    uint8_t sbuf[16];
    if (len < sizeof(sbuf)) {
        (void) read(sbuf, len, &res);
        return;
    }
    if (eof()) {
        return;
    }

    void *buf = calloc(1, std::min((size_t) PGP_INPUT_CACHE_SIZE, len));
    if (!buf) {
        error_ = true;
        return;
    }

    while (len && !eof()) {
        if (!read(buf, std::min((size_t) PGP_INPUT_CACHE_SIZE, len), &res)) {
            break;
        }
        len -= res;
    }
    free(buf);
}

rnp_result_t
pgp_source_t::finish()
{
    if (raw_finish) {
        return raw_finish(this);
    }
    return RNP_SUCCESS;
}

bool
pgp_source_t::error() const
{
    return error_;
}

bool
pgp_source_t::eof()
{
    if (eof_) {
        return true;
    }
    /* Error on stream read is NOT considered as eof. See error(). */
    uint8_t check;
    size_t  read = 0;
    return peek(&check, 1, &read) && (read == 0);
}

void
pgp_source_t::close()
{
    if (raw_close) {
        raw_close(this);
    }

    if (cache) {
        free(cache);
        cache = NULL;
    }
}

bool
pgp_source_t::skip_eol()
{
    uint8_t eol[2];
    size_t  read;

    if (!peek(eol, 2, &read) || !read) {
        return false;
    }
    if (eol[0] == '\n') {
        skip(1);
        return true;
    }
    if ((read == 2) && (eol[0] == '\r') && (eol[1] == '\n')) {
        skip(2);
        return true;
    }
    return false;
}

bool
pgp_source_t::skip_chars(const std::string &chars)
{
    do {
        char   ch = 0;
        size_t read = 0;
        if (!peek(&ch, 1, &read)) {
            return false;
        }
        if (!read) {
            /* return true only if there is no underlying read error */
            return true;
        }
        if (chars.find(ch) == std::string::npos) {
            return true;
        }
        skip(1);
    } while (1);
}

bool
pgp_source_t::peek_line(char *buf, size_t len, size_t *readres)
{
    size_t scan_pos = 0;
    size_t inc = 64;
    len = len - 1;

    do {
        size_t to_peek = scan_pos + inc;
        to_peek = to_peek > len ? len : to_peek;
        inc = inc * 2;

        /* inefficient, each time we again read from the beginning */
        if (!peek(buf, to_peek, readres)) {
            return false;
        }

        /* we continue scanning where we stopped previously */
        for (; scan_pos < *readres; scan_pos++) {
            if (buf[scan_pos] == '\n') {
                if ((scan_pos > 0) && (buf[scan_pos - 1] == '\r')) {
                    scan_pos--;
                }
                buf[scan_pos] = '\0';
                *readres = scan_pos;
                return true;
            }
        }
        if (*readres < to_peek) {
            return false;
        }
    } while (scan_pos < len);
    return false;
}

bool
init_src_common(pgp_source_t *src, size_t paramsize)
{
    memset(src, 0, sizeof(*src));
    src->cache = (pgp_source_cache_t *) calloc(1, sizeof(*src->cache));
    if (!src->cache) {
        RNP_LOG("cache allocation failed");
        return false;
    }
    src->cache->readahead = true;
    if (!paramsize) {
        return true;
    }
    src->param = calloc(1, paramsize);
    if (!src->param) {
        RNP_LOG("param allocation failed");
        free(src->cache);
        src->cache = NULL;
        return false;
    }
    return true;
}

typedef struct pgp_source_file_param_t {
    int fd;
} pgp_source_file_param_t;

static bool
file_src_read(pgp_source_t *src, void *buf, size_t len, size_t *readres)
{
    pgp_source_file_param_t *param = (pgp_source_file_param_t *) src->param;
    if (!param) {
        return false;
    }

    int64_t rres = read(param->fd, buf, len);
    if (rres < 0) {
        return false;
    }
    *readres = rres;
    return true;
}

static void
file_src_close(pgp_source_t *src)
{
    pgp_source_file_param_t *param = (pgp_source_file_param_t *) src->param;
    if (param) {
        if (src->type == PGP_STREAM_FILE) {
            close(param->fd);
        }
        free(src->param);
        src->param = NULL;
    }
}

static rnp_result_t
init_fd_src(pgp_source_t *src, int fd, uint64_t *size)
{
    if (!init_src_common(src, sizeof(pgp_source_file_param_t))) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    pgp_source_file_param_t *param = (pgp_source_file_param_t *) src->param;
    param->fd = fd;
    src->raw_read = file_src_read;
    src->raw_close = file_src_close;
    src->type = PGP_STREAM_FILE;
    src->size = size ? *size : 0;
    src->knownsize = !!size;

    return RNP_SUCCESS;
}

rnp_result_t
init_file_src(pgp_source_t *src, const char *path)
{
    int         fd;
    struct stat st;

    if (rnp_stat(path, &st) != 0) {
        RNP_LOG("can't stat '%s'", path);
        return RNP_ERROR_READ;
    }

    /* read call may succeed on directory depending on OS type */
    if (S_ISDIR(st.st_mode)) {
        RNP_LOG("source is directory");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    int flags = O_RDONLY;
#ifdef HAVE_O_BINARY
    flags |= O_BINARY;
#else
#ifdef HAVE__O_BINARY
    flags |= _O_BINARY;
#endif
#endif
    fd = rnp_open(path, flags, 0);

    if (fd < 0) {
        RNP_LOG("can't open '%s'", path);
        return RNP_ERROR_READ;
    }
    uint64_t     size = st.st_size;
    rnp_result_t ret = init_fd_src(src, fd, &size);
    if (ret) {
        close(fd);
    }
    return ret;
}

rnp_result_t
init_stdin_src(pgp_source_t *src)
{
    pgp_source_file_param_t *param;

    if (!init_src_common(src, sizeof(pgp_source_file_param_t))) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    param = (pgp_source_file_param_t *) src->param;
    param->fd = 0;
    src->raw_read = file_src_read;
    src->raw_close = file_src_close;
    src->type = PGP_STREAM_STDIN;

    return RNP_SUCCESS;
}

typedef struct pgp_source_mem_param_t {
    const void *memory;
    bool        free;
    size_t      len;
    size_t      pos;
} pgp_source_mem_param_t;

typedef struct pgp_dest_mem_param_t {
    unsigned maxalloc;
    unsigned allocated;
    void *   memory;
    bool     free;
    bool     discard_overflow;
    bool     secure;
} pgp_dest_mem_param_t;

static bool
mem_src_read(pgp_source_t *src, void *buf, size_t len, size_t *read)
{
    pgp_source_mem_param_t *param = (pgp_source_mem_param_t *) src->param;
    if (!param) {
        return false;
    }

    if (len > param->len - param->pos) {
        len = param->len - param->pos;
    }
    memcpy(buf, (uint8_t *) param->memory + param->pos, len);
    param->pos += len;
    *read = len;
    return true;
}

static void
mem_src_close(pgp_source_t *src)
{
    pgp_source_mem_param_t *param = (pgp_source_mem_param_t *) src->param;
    if (param) {
        if (param->free) {
            free((void *) param->memory);
        }
        free(src->param);
        src->param = NULL;
    }
}

rnp_result_t
init_mem_src(pgp_source_t *src, const void *mem, size_t len, bool free)
{
    if (!mem && len) {
        return RNP_ERROR_NULL_POINTER;
    }
    /* this is actually double buffering, but then src_peek will fail */
    if (!init_src_common(src, sizeof(pgp_source_mem_param_t))) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    pgp_source_mem_param_t *param = (pgp_source_mem_param_t *) src->param;
    param->memory = mem;
    param->len = len;
    param->pos = 0;
    param->free = free;
    src->raw_read = mem_src_read;
    src->raw_close = mem_src_close;
    src->raw_finish = NULL;
    src->size = len;
    src->knownsize = 1;
    src->type = PGP_STREAM_MEMORY;

    return RNP_SUCCESS;
}

static bool
null_src_read(pgp_source_t *src, void *buf, size_t len, size_t *read)
{
    return false;
}

rnp_result_t
init_null_src(pgp_source_t *src)
{
    memset(src, 0, sizeof(*src));
    src->raw_read = null_src_read;
    src->type = PGP_STREAM_NULL;
    src->error_ = true;
    return RNP_SUCCESS;
}

rnp_result_t
read_mem_src(pgp_source_t *src, pgp_source_t *readsrc)
{
    pgp_dest_t   dst;
    rnp_result_t ret = RNP_ERROR_GENERIC;
    size_t       sz = 0;

    if ((ret = init_mem_dest(&dst, NULL, 0))) {
        return ret;
    }

    if ((ret = dst_write_src(readsrc, &dst))) {
        goto done;
    }

    sz = dst.writeb;
    ret = init_mem_src(src, mem_dest_own_memory(&dst), sz, true);
done:
    dst_close(&dst, true);
    return ret;
}

const void *
mem_src_get_memory(pgp_source_t *src, bool own)
{
    if (src->type != PGP_STREAM_MEMORY) {
        RNP_LOG("wrong function call");
        return NULL;
    }

    if (!src->param) {
        return NULL;
    }

    pgp_source_mem_param_t *param = (pgp_source_mem_param_t *) src->param;
    if (own) {
        param->free = false;
    }
    return param->memory;
}

bool
init_dst_common(pgp_dest_t *dst, size_t paramsize)
{
    memset(dst, 0, sizeof(*dst));
    dst->werr = RNP_SUCCESS;
    if (!paramsize) {
        return true;
    }
    /* allocate param */
    dst->param = calloc(1, paramsize);
    if (!dst->param) {
        RNP_LOG("allocation failed");
    }
    return dst->param;
}

void
dst_write(pgp_dest_t *dst, const void *buf, size_t len)
{
    /* we call write function only if all previous calls succeeded */
    if ((len > 0) && (dst->write) && (dst->werr == RNP_SUCCESS)) {
        /* if cache non-empty and len will overflow it then fill it and write out */
        if ((dst->clen > 0) && (dst->clen + len > sizeof(dst->cache))) {
            memcpy(dst->cache + dst->clen, buf, sizeof(dst->cache) - dst->clen);
            buf = (uint8_t *) buf + sizeof(dst->cache) - dst->clen;
            len -= sizeof(dst->cache) - dst->clen;
            dst->werr = dst->write(dst, dst->cache, sizeof(dst->cache));
            dst->writeb += sizeof(dst->cache);
            dst->clen = 0;
            if (dst->werr != RNP_SUCCESS) {
                return;
            }
        }

        /* here everything will fit into the cache or cache is empty */
        if (dst->no_cache || (len > sizeof(dst->cache))) {
            dst->werr = dst->write(dst, buf, len);
            if (!dst->werr) {
                dst->writeb += len;
            }
        } else {
            memcpy(dst->cache + dst->clen, buf, len);
            dst->clen += len;
        }
    }
}

void
dst_write(pgp_dest_t &dst, const std::vector<uint8_t> &buf)
{
    dst_write(&dst, buf.data(), buf.size());
}

void
dst_printf(pgp_dest_t &dst, const char *format, ...)
{
    char    buf[2048];
    size_t  len;
    va_list ap;

    va_start(ap, format);
    len = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);

    if (len >= sizeof(buf)) {
        RNP_LOG("too long dst_printf");
        len = sizeof(buf) - 1;
    }
    dst_write(&dst, buf, len);
}

void
dst_flush(pgp_dest_t *dst)
{
    if ((dst->clen > 0) && (dst->write) && (dst->werr == RNP_SUCCESS)) {
        dst->werr = dst->write(dst, dst->cache, dst->clen);
        dst->writeb += dst->clen;
        dst->clen = 0;
    }
}

rnp_result_t
dst_finish(pgp_dest_t *dst)
{
    rnp_result_t res = RNP_SUCCESS;

    if (!dst->finished) {
        /* flush write cache in the dst */
        dst_flush(dst);
        if (dst->finish) {
            res = dst->finish(dst);
        }
        dst->finished = true;
    }

    return res;
}

void
dst_close(pgp_dest_t *dst, bool discard)
{
    if (!discard && !dst->finished) {
        dst_finish(dst);
    }

    if (dst->close) {
        dst->close(dst, discard);
    }
}

typedef struct pgp_dest_file_param_t {
    int         fd;
    int         errcode;
    bool        overwrite;
    std::string path;
} pgp_dest_file_param_t;

static rnp_result_t
file_dst_write(pgp_dest_t *dst, const void *buf, size_t len)
{
    pgp_dest_file_param_t *param = (pgp_dest_file_param_t *) dst->param;

    if (!param) {
        RNP_LOG("wrong param");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* we assyme that blocking I/O is used so everything is written or error received */
    ssize_t ret = write(param->fd, buf, len);
    if (ret < 0) {
        param->errcode = errno;
        RNP_LOG("write failed, error %d", param->errcode);
        return RNP_ERROR_WRITE;
    } else {
        param->errcode = 0;
        return RNP_SUCCESS;
    }
}

static void
file_dst_close(pgp_dest_t *dst, bool discard)
{
    pgp_dest_file_param_t *param = (pgp_dest_file_param_t *) dst->param;
    if (!param) {
        return;
    }

    if (dst->type == PGP_STREAM_FILE) {
        close(param->fd);
        if (discard) {
            rnp_unlink(param->path.c_str());
        }
    }

    delete param;
    dst->param = NULL;
}

static rnp_result_t
init_fd_dest(pgp_dest_t *dst, int fd, const char *path)
{
    if (!init_dst_common(dst, 0)) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    try {
        std::unique_ptr<pgp_dest_file_param_t> param(new pgp_dest_file_param_t());
        param->path = path;
        param->fd = fd;
        dst->param = param.release();
    } catch (const std::exception &e) {
        RNP_LOG("%s", e.what());
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    dst->write = file_dst_write;
    dst->close = file_dst_close;
    dst->type = PGP_STREAM_FILE;
    return RNP_SUCCESS;
}

rnp_result_t
init_file_dest(pgp_dest_t *dst, const char *path, bool overwrite)
{
    /* check whether file/dir already exists */
    struct stat st;
    if (!rnp_stat(path, &st)) {
        if (!overwrite) {
            RNP_LOG("file already exists: '%s'", path);
            return RNP_ERROR_WRITE;
        }

        /* if we are overwriting empty directory then should first remove it */
        if (S_ISDIR(st.st_mode)) {
            if (rmdir(path) == -1) {
                RNP_LOG("failed to remove directory: error %d", errno);
                return RNP_ERROR_BAD_PARAMETERS;
            }
        }
    }

    int flags = O_WRONLY | O_CREAT;
    flags |= overwrite ? O_TRUNC : O_EXCL;
#ifdef HAVE_O_BINARY
    flags |= O_BINARY;
#else
#ifdef HAVE__O_BINARY
    flags |= _O_BINARY;
#endif
#endif
    int fd = rnp_open(path, flags, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        RNP_LOG("failed to create file '%s'. Error %d.", path, errno);
        return RNP_ERROR_WRITE;
    }

    rnp_result_t res = init_fd_dest(dst, fd, path);
    if (res) {
        close(fd);
    }
    return res;
}

#define TMPDST_SUFFIX ".rnp-tmp.XXXXXX"

static rnp_result_t
file_tmpdst_finish(pgp_dest_t *dst)
{
    pgp_dest_file_param_t *param = (pgp_dest_file_param_t *) dst->param;
    if (!param) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* close the file */
    close(param->fd);
    param->fd = -1;

    /* rename the temporary file */
    if (param->path.size() < strlen(TMPDST_SUFFIX)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    try {
        /* remove suffix so we have required path */
        std::string origpath(param->path.begin(), param->path.end() - strlen(TMPDST_SUFFIX));
        /* check if file already exists */
        struct stat st;
        if (!rnp_stat(origpath.c_str(), &st)) {
            if (!param->overwrite) {
                RNP_LOG("target path already exists");
                return RNP_ERROR_BAD_STATE;
            }
#ifdef _WIN32
            /* rename() call on Windows fails if destination exists */
            else {
                rnp_unlink(origpath.c_str());
            }
#endif

            /* we should remove dir if overwriting, file will be unlinked in rename call */
            if (S_ISDIR(st.st_mode) && rmdir(origpath.c_str())) {
                RNP_LOG("failed to remove directory");
                return RNP_ERROR_BAD_STATE;
            }
        }

        if (rnp_rename(param->path.c_str(), origpath.c_str())) {
            RNP_LOG("failed to rename temporary path to target file: %s", strerror(errno));
            return RNP_ERROR_BAD_STATE;
        }
        return RNP_SUCCESS;
    } catch (const std::exception &e) {
        RNP_LOG("%s", e.what());
        return RNP_ERROR_BAD_STATE;
    }
}

static void
file_tmpdst_close(pgp_dest_t *dst, bool discard)
{
    pgp_dest_file_param_t *param = (pgp_dest_file_param_t *) dst->param;
    if (!param) {
        return;
    }

    /* we close file in finish function, except the case when some error occurred */
    if (!dst->finished && (dst->type == PGP_STREAM_FILE)) {
        close(param->fd);
        if (discard) {
            rnp_unlink(param->path.c_str());
        }
    }

    delete param;
    dst->param = NULL;
}

rnp_result_t
init_tmpfile_dest(pgp_dest_t *dst, const char *path, bool overwrite)
{
    try {
        std::string tmp = std::string(path) + std::string(TMPDST_SUFFIX);
        /* make sure tmp.data() is zero-terminated */
        tmp.push_back('\0');
#if defined(HAVE_MKSTEMP) && !defined(_WIN32)
        int fd = mkstemp(&tmp[0]);
#else
        int fd = rnp_mkstemp(&tmp[0]);
#endif
        if (fd < 0) {
            RNP_LOG("failed to create temporary file with template '%s'. Error %d.",
                    tmp.c_str(),
                    errno);
            return RNP_ERROR_WRITE;
        }
        rnp_result_t res = init_fd_dest(dst, fd, tmp.c_str());
        if (res) {
            close(fd);
            return res;
        }
    } catch (const std::exception &e) {
        RNP_LOG("%s", e.what());
        return RNP_ERROR_BAD_STATE;
    }

    /* now let's change some parameters to handle temporary file correctly */
    pgp_dest_file_param_t *param = (pgp_dest_file_param_t *) dst->param;
    param->overwrite = overwrite;
    dst->finish = file_tmpdst_finish;
    dst->close = file_tmpdst_close;
    return RNP_SUCCESS;
}

rnp_result_t
init_stdout_dest(pgp_dest_t *dst)
{
    rnp_result_t res = init_fd_dest(dst, STDOUT_FILENO, "");
    if (res) {
        return res;
    }
    dst->type = PGP_STREAM_STDOUT;
    return RNP_SUCCESS;
}

static rnp_result_t
mem_dst_write(pgp_dest_t *dst, const void *buf, size_t len)
{
    pgp_dest_mem_param_t *param = (pgp_dest_mem_param_t *) dst->param;
    if (!param) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* checking whether we need to realloc or discard extra bytes */
    if (param->discard_overflow && (dst->writeb >= param->allocated)) {
        return RNP_SUCCESS;
    }
    if (param->discard_overflow && (dst->writeb + len > param->allocated)) {
        len = param->allocated - dst->writeb;
    }

    if (dst->writeb + len > param->allocated) {
        if ((param->maxalloc > 0) && (dst->writeb + len > param->maxalloc)) {
            RNP_LOG("attempt to alloc more then allowed");
            return RNP_ERROR_OUT_OF_MEMORY;
        }

        /* round up to the page boundary and do it exponentially */
        size_t alloc = ((dst->writeb + len) * 2 + 4095) / 4096 * 4096;
        if ((param->maxalloc > 0) && (alloc > param->maxalloc)) {
            alloc = param->maxalloc;
        }

        void *newalloc = param->secure ? calloc(1, alloc) : realloc(param->memory, alloc);
        if (!newalloc) {
            return RNP_ERROR_OUT_OF_MEMORY;
        }
        if (param->secure && param->memory) {
            memcpy(newalloc, param->memory, dst->writeb);
            secure_clear(param->memory, dst->writeb);
            free(param->memory);
        }
        param->memory = newalloc;
        param->allocated = alloc;
    }

    memcpy((uint8_t *) param->memory + dst->writeb, buf, len);
    return RNP_SUCCESS;
}

static void
mem_dst_close(pgp_dest_t *dst, bool discard)
{
    pgp_dest_mem_param_t *param = (pgp_dest_mem_param_t *) dst->param;
    if (!param) {
        return;
    }

    if (param->free) {
        if (param->secure) {
            secure_clear(param->memory, param->allocated);
        }
        free(param->memory);
    }
    free(param);
    dst->param = NULL;
}

rnp_result_t
init_mem_dest(pgp_dest_t *dst, void *mem, unsigned len)
{
    pgp_dest_mem_param_t *param;

    if (!init_dst_common(dst, sizeof(*param))) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    param = (pgp_dest_mem_param_t *) dst->param;

    param->maxalloc = len;
    param->allocated = mem ? len : 0;
    param->memory = mem;
    param->free = !mem;
    param->secure = false;

    dst->write = mem_dst_write;
    dst->close = mem_dst_close;
    dst->type = PGP_STREAM_MEMORY;
    dst->werr = RNP_SUCCESS;
    dst->no_cache = true;

    return RNP_SUCCESS;
}

void
mem_dest_discard_overflow(pgp_dest_t *dst, bool discard)
{
    if (dst->type != PGP_STREAM_MEMORY) {
        RNP_LOG("wrong function call");
        return;
    }

    pgp_dest_mem_param_t *param = (pgp_dest_mem_param_t *) dst->param;
    if (param) {
        param->discard_overflow = discard;
    }
}

void *
mem_dest_get_memory(pgp_dest_t *dst)
{
    if (dst->type != PGP_STREAM_MEMORY) {
        RNP_LOG("wrong function call");
        return NULL;
    }

    pgp_dest_mem_param_t *param = (pgp_dest_mem_param_t *) dst->param;

    if (param) {
        return param->memory;
    }

    return NULL;
}

void *
mem_dest_own_memory(pgp_dest_t *dst)
{
    if (dst->type != PGP_STREAM_MEMORY) {
        RNP_LOG("wrong function call");
        return NULL;
    }

    pgp_dest_mem_param_t *param = (pgp_dest_mem_param_t *) dst->param;

    if (!param) {
        RNP_LOG("null param");
        return NULL;
    }

    dst_finish(dst);

    if (param->free) {
        if (!dst->writeb) {
            free(param->memory);
            param->memory = NULL;
            return param->memory;
        }
        /* it may be larger then required - let's truncate */
        void *newalloc = realloc(param->memory, dst->writeb);
        if (!newalloc) {
            return NULL;
        }
        param->memory = newalloc;
        param->allocated = dst->writeb;
        param->free = false;
        return param->memory;
    }

    /* in this case we should copy the memory */
    void *res = malloc(dst->writeb);
    if (res) {
        memcpy(res, param->memory, dst->writeb);
    }
    return res;
}

void
mem_dest_secure_memory(pgp_dest_t *dst, bool secure)
{
    if (!dst || (dst->type != PGP_STREAM_MEMORY)) {
        RNP_LOG("wrong function call");
        return;
    }
    pgp_dest_mem_param_t *param = (pgp_dest_mem_param_t *) dst->param;
    if (param) {
        param->secure = secure;
    }
}

static rnp_result_t
null_dst_write(pgp_dest_t *dst, const void *buf, size_t len)
{
    return RNP_SUCCESS;
}

static void
null_dst_close(pgp_dest_t *dst, bool discard)
{
    ;
}

rnp_result_t
init_null_dest(pgp_dest_t *dst)
{
    dst->param = NULL;
    dst->write = null_dst_write;
    dst->close = null_dst_close;
    dst->type = PGP_STREAM_NULL;
    dst->writeb = 0;
    dst->clen = 0;
    dst->werr = RNP_SUCCESS;
    dst->no_cache = true;

    return RNP_SUCCESS;
}

rnp_result_t
dst_write_src(pgp_source_t *src, pgp_dest_t *dst, uint64_t limit)
{
    const size_t bufsize = PGP_INPUT_CACHE_SIZE;
    uint8_t *    readbuf = (uint8_t *) malloc(bufsize);
    if (!readbuf) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }
    rnp_result_t res = RNP_SUCCESS;
    try {
        size_t   read;
        uint64_t totalread = 0;

        while (!src->eof_) {
            if (!src->read(readbuf, bufsize, &read)) {
                res = RNP_ERROR_GENERIC;
                break;
            }
            if (!read) {
                continue;
            }
            totalread += read;
            if (limit && totalread > limit) {
                res = RNP_ERROR_GENERIC;
                break;
            }
            if (dst) {
                dst_write(dst, readbuf, read);
                if (dst->werr) {
                    RNP_LOG("failed to output data");
                    res = RNP_ERROR_WRITE;
                    break;
                }
            }
        }
    } catch (...) {
        free(readbuf);
        throw;
    }
    free(readbuf);
    if (res || !dst) {
        return res;
    }
    dst_flush(dst);
    return dst->werr;
}

bool
have_pkesk_checksum(pgp_pubkey_alg_t alg)
{
    switch (alg) {
#if defined(ENABLE_CRYPTO_REFRESH) || defined(ENABLE_PQC)
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_PKA_X25519:
#endif
#if defined(ENABLE_PQC)
    case PGP_PKA_KYBER768_X25519:
    // case PGP_PKA_KYBER1024_X448:
    case PGP_PKA_KYBER768_P256:
    case PGP_PKA_KYBER1024_P384:
    case PGP_PKA_KYBER768_BP256:
    case PGP_PKA_KYBER1024_BP384:
#endif
        return false;
#endif
    default:
        return true;
    }
}

bool
do_encrypt_pkesk_v3_alg_id(pgp_pubkey_alg_t alg)
{
    /* matches the same algorithms */
    return have_pkesk_checksum(alg);
}

#if defined(ENABLE_CRYPTO_REFRESH) || defined(ENABLE_PQC)
/* The crypto refresh mandates that for a X25519/X448 PKESKv3, AES MUST be used.
   The same is true for the PQC algorithms
 */
bool
check_enforce_aes_v3_pkesk(pgp_pubkey_alg_t alg, pgp_symm_alg_t salg, pgp_pkesk_version_t ver)
{
    /* The same algorithms as with pkesk_checksum */
    return (ver != PGP_PKSK_V3) || have_pkesk_checksum(alg) || pgp_is_sa_aes(salg);
}
#endif

#if defined(ENABLE_AEAD)
bool
encrypted_sesk_set_ad(pgp_crypt_t &crypt, pgp_sk_sesskey_t &skey)
{
    uint8_t ad_data[4];

    ad_data[0] = PGP_PKT_SK_SESSION_KEY | PGP_PTAG_ALWAYS_SET | PGP_PTAG_NEW_FORMAT;
    ad_data[1] = skey.version;
    ad_data[2] = skey.alg;
    ad_data[3] = skey.aalg;

    return pgp_cipher_aead_set_ad(&crypt, ad_data, 4);
}
#endif
