/*
 * Copyright (c) 2017-2023 [Ribose Inc](https://www.ribose.com).
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

#include "support.h"
#include "rnp_tests.h"
#include "str-utils.h"
#include <librepgp/stream-ctx.h>
#include "key.hpp"
#include "librepgp/stream-armor.h"
#include "ffi-priv-types.h"

#ifdef _MSC_VER
#include "uniwin.h"
#include <shlwapi.h>
#else
#include <sys/types.h>
#include <sys/param.h>
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>

#include <key.hpp>
#include <fstream>
#include <vector>
#include <algorithm>

#ifndef WINSHELLAPI
#include <ftw.h>
#endif

#ifdef _WIN32
int
setenv(const char *name, const char *value, int overwrite)
{
    if (getenv(name) && !overwrite) {
        return 0;
    }
    char varbuf[512] = {0};
    snprintf(varbuf, sizeof(varbuf) - 1, "%s=%s", name, value);
    return _putenv(varbuf);
}

int
unsetenv(const char *name)
{
    char varbuf[512] = {0};
    snprintf(varbuf, sizeof(varbuf) - 1, "%s=", name);
    return _putenv(varbuf);
}
#endif

std::string
file_to_str(const std::string &path)
{
    // TODO: wstring path _WIN32
    std::ifstream infile(path);
    return std::string(std::istreambuf_iterator<char>(infile),
                       std::istreambuf_iterator<char>());
}

std::vector<uint8_t>
file_to_vec(const std::string &path)
{
    // TODO: wstring path _WIN32
    std::ifstream stream(path, std::ios::in | std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(stream)),
                                std::istreambuf_iterator<char>());
}

void
str_to_file(const std::string &path, const char *str)
{
    std::ofstream stream(path, std::ios::out | std::ios::binary);
    stream.write(str, strlen(str));
}

off_t
file_size(const char *path)
{
    struct stat path_stat;
    if (rnp_stat(path, &path_stat) != -1) {
        if (S_ISDIR(path_stat.st_mode)) {
            return -1;
        }
        return path_stat.st_size;
    }
    return -1;
}

/* Concatenate multiple strings into a full path.
 * A directory separator is added between components.
 * Must be called in between va_start and va_end.
 * Final argument of calling function must be NULL.
 */
void
vpaths_concat(char *buffer, size_t buffer_size, const char *first, va_list ap)
{
    size_t      length = strlen(first);
    const char *s;

    assert_true(length < buffer_size);

    memset(buffer, 0, buffer_size);

    strncpy(buffer, first, buffer_size - 1);
    while ((s = va_arg(ap, const char *))) {
        length += strlen(s) + 1;
        assert_true(length < buffer_size);
        strncat(buffer, "/", buffer_size - 1);
        strncat(buffer, s, buffer_size - 1);
    }
}

/* Concatenate multiple strings into a full path.
 * Final argument must be NULL.
 */
char *
paths_concat(char *buffer, size_t buffer_length, const char *first, ...)
{
    va_list ap;

    va_start(ap, first);
    vpaths_concat(buffer, buffer_length, first, ap);
    va_end(ap);
    return buffer;
}

/* Concatenate multiple strings into a full path and
 * check that the file exists.
 * Final argument must be NULL.
 */
int
path_rnp_file_exists(const char *first, ...)
{
    va_list ap;
    char    buffer[512] = {0};

    va_start(ap, first);
    vpaths_concat(buffer, sizeof(buffer), first, ap);
    va_end(ap);
    return rnp_file_exists(buffer);
}

/* Concatenate multiple strings into a full path and
 * create the directory.
 * Final argument must be NULL.
 */
void
path_mkdir(mode_t mode, const char *first, ...)
{
    va_list ap;
    char    buffer[512];

    va_start(ap, first);
    vpaths_concat(buffer, sizeof(buffer), first, ap);
    va_end(ap);

    assert_int_equal(0, RNP_MKDIR(buffer, mode));
}

#ifndef WINSHELLAPI
static int
remove_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    int ret = remove(fpath);
    if (ret)
        perror(fpath);

    return ret;
}
#endif

static const char *
get_tmp()
{
    const char *tmp = getenv("TEMP");
    return tmp ? tmp : "/tmp";
}

static bool
is_tmp_path(const char *path)
{
    char *rlpath = realpath(path, NULL);
    if (!rlpath) {
        rlpath = strdup(path);
    }
    const char *tmp = get_tmp();
    char *      rltmp = realpath(tmp, NULL);
    if (!rltmp) {
        rltmp = strdup(tmp);
    }
    bool res = rlpath && rltmp && !strncmp(rlpath, rltmp, strlen(rltmp));
    free(rlpath);
    free(rltmp);
    return res;
}

/* Recursively remove a directory.
 * The path must be located in /tmp, for safety.
 */
void
delete_recursively(const char *path)
{
    bool relative =
#ifdef _MSC_VER
      PathIsRelativeA(path);
#else
      *path != '/';
#endif
    std::string fullpath = path;
    if (relative) {
        char *cwd = getcwd(NULL, 0);
        fullpath = rnp::path::append(cwd, fullpath);
        free(cwd);
    }
    /* sanity check, we should only be purging things from /tmp/ */
    assert_true(is_tmp_path(fullpath.c_str()));

#ifdef WINSHELLAPI
    SHFILEOPSTRUCTA fileOp = {};
    fileOp.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;
    assert_true(fullpath.size() < MAX_PATH);
    char newFrom[MAX_PATH + 1];
    strcpy_s(newFrom, fullpath.c_str());
    newFrom[fullpath.size() + 1] = NULL; // two NULLs are required
    fileOp.pFrom = newFrom;
    fileOp.pTo = NULL;
    fileOp.wFunc = FO_DELETE;
    fileOp.hNameMappings = NULL;
    fileOp.hwnd = NULL;
    fileOp.lpszProgressTitle = NULL;
    SHFileOperationA(&fileOp);
#else
    nftw(path, remove_cb, 64, FTW_DEPTH | FTW_PHYS);
#endif
}

void
copy_recursively(const char *src, const char *dst)
{
    assert_true(src != nullptr);
    /* sanity check, we should only be copying things to /tmp/ */
    assert_true(is_tmp_path(dst));

#ifdef WINSHELLAPI
    SHFILEOPSTRUCTA fileOp = {};
    fileOp.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR;
    fileOp.pFrom = src;
    fileOp.pTo = dst;
    assert_true(strlen(src) < MAX_PATH);
    char newFrom[MAX_PATH + 1];
    strcpy_s(newFrom, src);
    newFrom[strlen(src) + 1] = NULL; // two NULLs are required
    fileOp.pFrom = newFrom;
    assert_true(strlen(dst) < MAX_PATH);
    char newTo[MAX_PATH + 1];
    strcpy_s(newTo, dst);
    newTo[strlen(dst) + 1] = NULL; // two NULLs are required
    fileOp.wFunc = FO_COPY;
    fileOp.hNameMappings = NULL;
    fileOp.hwnd = NULL;
    fileOp.lpszProgressTitle = NULL;
    assert_int_equal(0, SHFileOperationA(&fileOp));
#else
    // TODO: maybe use fts or something less hacky
    char buf[2048];
#ifndef _WIN32
    snprintf(buf, sizeof(buf), "cp -pRP '%s' '%s'", src, dst);
#else
    snprintf(buf, sizeof(buf), "xcopy \"%s\" \"%s\" /I /Q /E /Y", src, dst);
#endif // _WIN32
    assert_int_equal(0, system(buf));
#endif // WINSHELLAPI
}

/* Creates and returns a temporary directory path.
 * Caller must free the string.
 */
#if defined(HAVE_MKDTEMP)
char *
make_temp_dir()
{
    char rltmp[PATH_MAX] = {0};
    if (!realpath(get_tmp(), rltmp)) {
        printf("Fatal: realpath on tmp folder failed. Error %d.\n", errno);
        return NULL;
    }

    auto  atemplate = "/rnp-gtest-XXXXXX";
    char *buffer = (char *) calloc(1, strlen(rltmp) + strlen(atemplate) + 1);
    if (!buffer) {
        return NULL;
    }
    memcpy(buffer, rltmp, strlen(rltmp));
    memcpy(buffer + strlen(rltmp), atemplate, strlen(atemplate));
    buffer[strlen(rltmp) + strlen(atemplate)] = '\0';
    char *res = mkdtemp(buffer);
    if (!res) {
        free(buffer);
    }
    return res;
}
#elif defined(HAVE__TEMPNAM)
char *
make_temp_dir()
{
    const int MAX_ATTEMPTS = 10;
    for (int i = 0; i < MAX_ATTEMPTS; i++) {
        char *dir = _tempnam(NULL, "rnp-gtest-");
        if (!dir) {
            fprintf(stderr, "_tempnam failed to generate temporary path");
            continue;
        }
        if (RNP_MKDIR(dir, S_IRWXU)) {
            fprintf(stderr, "Failed to create temporary directory");
            free(dir);
            continue;
        }
        return dir;
    }
    fprintf(stderr, "Failed to make temporary directory, aborting");
    return NULL;
}
#else
#error Unsupported platform
#endif

void
clean_temp_dir(const char *path)
{
    if (!getenv("RNP_KEEP_TEMP")) {
        delete_recursively(path);
    }
}

bool
bin_eq_hex(const uint8_t *data, size_t len, const char *val)
{
    auto valbin = rnp::hex_to_bin(val);
    return (valbin.size() == len) && !memcmp(data, valbin.data(), len);
}

bool
hex2mpi(pgp::mpi *val, const char *hex)
{
    auto hexbin = rnp::hex_to_bin(hex);
    val->assign(hexbin.data(), hexbin.size());
    return true;
}

bool
cmp_keyid(const pgp::KeyID &id, const std::string &val)
{
    return bin_eq_hex(id.data(), id.size(), val.c_str());
}

bool
cmp_keyfp(const pgp::Fingerprint &fp, const std::string &val)
{
    return bin_eq_hex(fp.data(), fp.size(), val.c_str());
}

void
test_ffi_init(rnp_ffi_t *ffi)
{
    // setup FFI
    assert_rnp_success(rnp_ffi_create(ffi, "GPG", "GPG"));
    // load our keyrings
    assert_true(
      load_keys_gpg(*ffi, "data/keyrings/1/pubring.gpg", "data/keyrings/1/secring.gpg"));
}

bool
mpi_empty(const pgp::mpi &val)
{
    return val.size() == 0;
}

bool
write_pass_to_pipe(int fd, size_t count)
{
    const char *const password = "passwordforkeygeneration\n";
    for (size_t i = 0; i < count; i++) {
        const char *p = password;
        ssize_t     remaining = strlen(p);

        do {
            ssize_t written = write(fd, p, remaining);
            if (written <= 0) {
                perror("write");
                return false;
            }
            p += written;
            remaining -= written;
        } while (remaining);
    }
    return true;
}

bool
setupPasswordfd(int *pipefd)
{
    bool ok = false;

    if (pipe(pipefd) == -1) {
        perror("pipe");
        goto end;
    }
    // write it twice for normal keygen (primary+sub)
    if (!write_pass_to_pipe(pipefd[1], 2)) {
        close(pipefd[1]);
        goto end;
    }
    ok = true;

end:
    close(pipefd[1]);
    return ok;
}

static bool
setup_rnp_cfg(rnp_cfg &cfg, const char *ks_format, const char *homedir, int *pipefd)
{
    bool res;
    char pubpath[MAXPATHLEN];
    char secpath[MAXPATHLEN];
    char homepath[MAXPATHLEN];

    /* set password fd if any */
    if (pipefd) {
        if (!(res = setupPasswordfd(pipefd))) {
            return res;
        }
        cfg.set_int(CFG_PASSFD, pipefd[0]);
        // pipefd[0] will be closed via passfp
        pipefd[0] = -1;
    }
    /* setup keyring paths */
    if (homedir == NULL) {
        /* if we use default homedir then we append '.rnp' and create directory as well */
        homedir = getenv("HOME");
        paths_concat(homepath, sizeof(homepath), homedir, ".rnp", NULL);
        if (!rnp_dir_exists(homepath)) {
            path_mkdir(0700, homepath, NULL);
        }
        homedir = homepath;
    }

    if (homedir == NULL) {
        return false;
    }

    cfg.set_str(CFG_KR_PUB_FORMAT, ks_format);
    cfg.set_str(CFG_KR_SEC_FORMAT, ks_format);

    if (strcmp(ks_format, RNP_KEYSTORE_GPG) == 0) {
        paths_concat(pubpath, MAXPATHLEN, homedir, PUBRING_GPG, NULL);
        paths_concat(secpath, MAXPATHLEN, homedir, SECRING_GPG, NULL);
    } else if (strcmp(ks_format, RNP_KEYSTORE_KBX) == 0) {
        paths_concat(pubpath, MAXPATHLEN, homedir, PUBRING_KBX, NULL);
        paths_concat(secpath, MAXPATHLEN, homedir, SECRING_KBX, NULL);
    } else if (strcmp(ks_format, RNP_KEYSTORE_G10) == 0) {
        paths_concat(pubpath, MAXPATHLEN, homedir, PUBRING_G10, NULL);
        paths_concat(secpath, MAXPATHLEN, homedir, SECRING_G10, NULL);
    } else if (strcmp(ks_format, RNP_KEYSTORE_GPG21) == 0) {
        paths_concat(pubpath, MAXPATHLEN, homedir, PUBRING_KBX, NULL);
        paths_concat(secpath, MAXPATHLEN, homedir, SECRING_G10, NULL);
        cfg.set_str(CFG_KR_PUB_FORMAT, RNP_KEYSTORE_KBX);
        cfg.set_str(CFG_KR_SEC_FORMAT, RNP_KEYSTORE_G10);
    } else {
        return false;
    }

    cfg.set_str(CFG_KR_PUB_PATH, (char *) pubpath);
    cfg.set_str(CFG_KR_SEC_PATH, (char *) secpath);
    return true;
}

bool
setup_cli_rnp_common(cli_rnp_t *rnp, const char *ks_format, const char *homedir, int *pipefd)
{
    rnp_cfg cfg;
    if (!setup_rnp_cfg(cfg, ks_format, homedir, pipefd)) {
        return false;
    }

    /*initialize the basic RNP structure. */
    return rnp->init(cfg);
}

void
cli_set_default_rsa_key_desc(rnp_cfg &cfg, const char *hashalg)
{
    cfg.set_int(CFG_NUMBITS, 1024);
    cfg.set_str(CFG_HASH, hashalg);
    cfg.set_int(CFG_S2K_ITER, 1);
    cli_rnp_set_generate_params(cfg);
}

// this is a password callback that will always fail
bool
failing_password_callback(const pgp_password_ctx_t *ctx,
                          char *                    password,
                          size_t                    password_size,
                          void *                    userdata)
{
    return false;
}

bool
ffi_failing_password_provider(rnp_ffi_t        ffi,
                              void *           app_ctx,
                              rnp_key_handle_t key,
                              const char *     pgp_context,
                              char *           buf,
                              size_t           buf_len)
{
    return false;
}

bool
ffi_asserting_password_provider(rnp_ffi_t        ffi,
                                void *           app_ctx,
                                rnp_key_handle_t key,
                                const char *     pgp_context,
                                char *           buf,
                                size_t           buf_len)
{
    EXPECT_TRUE(false);
    return false;
}

bool
ffi_string_password_provider(rnp_ffi_t        ffi,
                             void *           app_ctx,
                             rnp_key_handle_t key,
                             const char *     pgp_context,
                             char *           buf,
                             size_t           buf_len)
{
    size_t pass_len = strlen((const char *) app_ctx);
    if (pass_len >= buf_len) {
        return false;
    }
    memcpy(buf, app_ctx, pass_len + 1);
    return true;
}

// this is a password callback that should never be called
bool
asserting_password_callback(const pgp_password_ctx_t *ctx,
                            char *                    password,
                            size_t                    password_size,
                            void *                    userdata)
{
    EXPECT_TRUE(false);
    return false;
}

// this is a password callback that just copies the string in userdata to
// the password buffer
bool
string_copy_password_callback(const pgp_password_ctx_t *ctx,
                              char *                    password,
                              size_t                    password_size,
                              void *                    userdata)
{
    const char *str = (const char *) userdata;
    strncpy(password, str, password_size - 1);
    return true;
}

void
unused_getkeycb(rnp_ffi_t   ffi,
                void *      app_ctx,
                const char *identifier_type,
                const char *identifier,
                bool        secret)
{
    EXPECT_TRUE(false);
}

bool
unused_getpasscb(rnp_ffi_t        ffi,
                 void *           app_ctx,
                 rnp_key_handle_t key,
                 const char *     pgp_context,
                 char *           buf,
                 size_t           buf_len)
{
    EXPECT_TRUE(false);
    return false;
}

bool
starts_with(const std::string &data, const std::string &match)
{
    return data.find(match) == 0;
}

bool
ends_with(const std::string &data, const std::string &match)
{
    return data.size() >= match.size() &&
           data.substr(data.size() - match.size(), match.size()) == match;
}

std::string
fmt(const char *format, ...)
{
    int     size;
    va_list ap;

    va_start(ap, format);
    size = vsnprintf(NULL, 0, format, ap);
    va_end(ap);

    // +1 for terminating null
    std::string buf(size + 1, '\0');

    va_start(ap, format);
    size = vsnprintf(&buf[0], buf.size(), format, ap);
    va_end(ap);

    // drop terminating null
    buf.resize(size);
    return buf;
}

std::string
strip_eol(const std::string &str)
{
    size_t endpos = str.find_last_not_of("\r\n");
    if (endpos != std::string::npos) {
        return str.substr(0, endpos + 1);
    }
    return str;
}

std::string
lowercase(const std::string &str)
{
    std::string res = str;
    std::transform(
      res.begin(), res.end(), res.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return res;
}

static bool
jso_get_field(json_object *obj, json_object **fld, const std::string &name)
{
    if (!obj || !json_object_is_type(obj, json_type_object)) {
        return false;
    }
    return json_object_object_get_ex(obj, name.c_str(), fld);
}

bool
check_json_field_str(json_object *obj, const std::string &field, const std::string &value)
{
    json_object *fld = NULL;
    if (!jso_get_field(obj, &fld, field)) {
        return false;
    }
    if (!json_object_is_type(fld, json_type_string)) {
        return false;
    }
    const char *jsoval = json_object_get_string(fld);
    return jsoval && (value == jsoval);
}

bool
check_json_field_int(json_object *obj, const std::string &field, int value)
{
    json_object *fld = NULL;
    if (!jso_get_field(obj, &fld, field)) {
        return false;
    }
    if (!json_object_is_type(fld, json_type_int)) {
        return false;
    }
    return json_object_get_int(fld) == value;
}

bool
check_json_field_bool(json_object *obj, const std::string &field, bool value)
{
    json_object *fld = NULL;
    if (!jso_get_field(obj, &fld, field)) {
        return false;
    }
    if (!json_object_is_type(fld, json_type_boolean)) {
        return false;
    }
    // 'json_object_get_boolean' returns 'json_bool' which is 'int' on Windows
    // but bool on other platforms
    return (json_object_get_boolean(fld) ? true : false) == value;
}

bool
check_json_pkt_type(json_object *pkt, int tag)
{
    if (!pkt || !json_object_is_type(pkt, json_type_object)) {
        return false;
    }
    json_object *hdr = NULL;
    if (!json_object_object_get_ex(pkt, "header", &hdr)) {
        return false;
    }
    if (!json_object_is_type(hdr, json_type_object)) {
        return false;
    }
    return check_json_field_int(hdr, "tag", tag);
}

rnp::Key *
rnp_tests_get_key_by_id(rnp::KeyStore *keyring, const std::string &keyid, rnp::Key *after)
{
    if (!keyring || keyid.empty() || !rnp::is_hex(keyid)) {
        return NULL;
    }
    pgp::KeyID keyid_bin = {};
    size_t     binlen = rnp::hex_decode(keyid.c_str(), keyid_bin.data(), keyid_bin.size());
    if (binlen > PGP_KEY_ID_SIZE) {
        return NULL;
    }
    rnp::KeyIDSearch search(keyid_bin);
    return keyring->search(search, after);
}

rnp::Key *
rnp_tests_get_key_by_grip(rnp::KeyStore *keyring, const std::string &grip)
{
    if (!keyring || grip.empty() || !rnp::is_hex(grip)) {
        return NULL;
    }
    pgp::KeyGrip grip_bin{};
    size_t       binlen = rnp::hex_decode(grip.c_str(), grip_bin.data(), grip_bin.size());
    if (binlen > PGP_KEY_GRIP_SIZE) {
        return NULL;
    }
    return rnp_tests_get_key_by_grip(keyring, grip_bin);
}

rnp::Key *
rnp_tests_get_key_by_grip(rnp::KeyStore *keyring, const pgp::KeyGrip &grip)
{
    if (!keyring) {
        return NULL;
    }
    return keyring->search(rnp::KeyGripSearch(grip));
}

rnp::Key *
rnp_tests_get_key_by_fpr(rnp::KeyStore *keyring, const std::string &fpstr)
{
    if (!keyring || fpstr.empty() || !rnp::is_hex(fpstr)) {
        return NULL;
    }
    std::vector<uint8_t> fp_bin(PGP_MAX_FINGERPRINT_SIZE, 0);
    size_t               binlen = rnp::hex_decode(fpstr.c_str(), fp_bin.data(), fp_bin.size());
    if (binlen > PGP_MAX_FINGERPRINT_SIZE) {
        return NULL;
    }
    pgp::Fingerprint fp(fp_bin.data(), binlen);
    return keyring->get_key(fp);
}

rnp::Key *
rnp_tests_key_search(rnp::KeyStore *keyring, const std::string &uid)
{
    if (!keyring || uid.empty()) {
        return NULL;
    }
    return keyring->search(rnp::KeyUIDSearch(uid));
}

void
reload_pubring(rnp_ffi_t *ffi)
{
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_save_keys(*ffi, "GPG", output, RNP_LOAD_SAVE_PUBLIC_KEYS));
    assert_rnp_success(rnp_ffi_destroy(*ffi));

    /* get output */
    uint8_t *buf = NULL;
    size_t   len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    rnp_input_t input = NULL;
    assert_rnp_success(rnp_input_from_memory(&input, buf, len, false));

    /* re-init ffi and load keys */
    assert_rnp_success(rnp_ffi_create(ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_import_keys(*ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS, NULL));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_input_destroy(input));
}

void
reload_keyrings(rnp_ffi_t *ffi)
{
    rnp_output_t outpub = NULL;
    assert_rnp_success(rnp_output_to_memory(&outpub, 0));
    assert_rnp_success(rnp_save_keys(*ffi, "GPG", outpub, RNP_LOAD_SAVE_PUBLIC_KEYS));
    rnp_output_t outsec = NULL;
    assert_rnp_success(rnp_output_to_memory(&outsec, 0));
    assert_rnp_success(rnp_save_keys(*ffi, "GPG", outsec, RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_ffi_destroy(*ffi));
    /* re-init ffi and load keys */
    assert_rnp_success(rnp_ffi_create(ffi, "GPG", "GPG"));

    uint8_t *buf = NULL;
    size_t   len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(outpub, &buf, &len, false));
    rnp_input_t input = NULL;
    assert_rnp_success(rnp_input_from_memory(&input, buf, len, false));
    assert_rnp_success(rnp_import_keys(*ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS, NULL));
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(outpub));

    assert_rnp_success(rnp_output_memory_get_buf(outsec, &buf, &len, false));
    assert_rnp_success(rnp_input_from_memory(&input, buf, len, false));
    assert_rnp_success(rnp_import_keys(*ffi, input, RNP_LOAD_SAVE_SECRET_KEYS, NULL));
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(outsec));
}

static bool
load_keys_internal(rnp_ffi_t          ffi,
                   const std::string &format,
                   const std::string &path,
                   bool               secret)
{
    if (path.empty()) {
        return true;
    }
    rnp_input_t input = NULL;
    if (rnp_input_from_path(&input, path.c_str())) {
        return false;
    }
    bool res = !rnp_load_keys(ffi,
                              format.c_str(),
                              input,
                              secret ? RNP_LOAD_SAVE_SECRET_KEYS : RNP_LOAD_SAVE_PUBLIC_KEYS);
    rnp_input_destroy(input);
    return res;
}

bool
load_keys_gpg(rnp_ffi_t ffi, const std::string &pub, const std::string &sec)
{
    return load_keys_internal(ffi, "GPG", pub, false) &&
           load_keys_internal(ffi, "GPG", sec, true);
}

bool
load_keys_kbx_g10(rnp_ffi_t ffi, const std::string &pub, const std::string &sec)
{
    return load_keys_internal(ffi, "KBX", pub, false) &&
           load_keys_internal(ffi, "G10", sec, true);
}

static bool
import_keys(rnp_ffi_t ffi, const std::string &path, uint32_t flags)
{
    rnp_input_t input = NULL;
    if (rnp_input_from_path(&input, path.c_str())) {
        return false;
    }
    bool res = !rnp_import_keys(ffi, input, flags, NULL);
    rnp_input_destroy(input);
    return res;
}

bool
import_all_keys(rnp_ffi_t ffi, const std::string &path)
{
    return import_keys(ffi, path, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS);
}

bool
import_pub_keys(rnp_ffi_t ffi, const std::string &path)
{
    return import_keys(ffi, path, RNP_LOAD_SAVE_PUBLIC_KEYS);
}

bool
import_sec_keys(rnp_ffi_t ffi, const std::string &path)
{
    return import_keys(ffi, path, RNP_LOAD_SAVE_SECRET_KEYS);
}

static bool
import_keys(rnp_ffi_t ffi, const uint8_t *data, size_t len, uint32_t flags)
{
    rnp_input_t input = NULL;
    if (rnp_input_from_memory(&input, data, len, false)) {
        return false;
    }
    bool res = !rnp_import_keys(ffi, input, flags, NULL);
    rnp_input_destroy(input);
    return res;
}

bool
import_all_keys(rnp_ffi_t ffi, const uint8_t *data, size_t len, uint32_t flags)
{
    return import_keys(
      ffi, data, len, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS | flags);
}

bool
import_pub_keys(rnp_ffi_t ffi, const uint8_t *data, size_t len)
{
    return import_keys(ffi, data, len, RNP_LOAD_SAVE_PUBLIC_KEYS);
}

bool
import_sec_keys(rnp_ffi_t ffi, const uint8_t *data, size_t len)
{
    return import_keys(ffi, data, len, RNP_LOAD_SAVE_SECRET_KEYS);
}

std::vector<uint8_t>
export_key(rnp_key_handle_t key, bool armored, bool secret)
{
    uint32_t flags = RNP_KEY_EXPORT_SUBKEYS;
    if (armored) {
        flags = flags | RNP_KEY_EXPORT_ARMORED;
    }
    if (secret) {
        flags = flags | RNP_KEY_EXPORT_SECRET;
    } else {
        flags = flags | RNP_KEY_EXPORT_PUBLIC;
    }
    rnp_output_t output = NULL;
    rnp_output_to_memory(&output, 0);
    rnp_key_export(key, output, flags);
    size_t   len = 0;
    uint8_t *buf = NULL;
    rnp_output_memory_get_buf(output, &buf, &len, false);
    std::vector<uint8_t> res(buf, buf + len);
    rnp_output_destroy(output);
    return res;
}

#if 0
void
dump_key_stdout(rnp_key_handle_t key, bool secret)
{
    auto pub = export_key(key, true, false);
    printf("%.*s", (int) pub.size(), (char *) pub.data());
    if (!secret) {
        return;
    }
    auto sec = export_key(key, true, true);
    printf("%.*s", (int) sec.size(), (char *) sec.data());
}
#endif

bool
write_transferable_key(pgp_transferable_key_t &key, pgp_dest_t &dst, bool armor)
{
    pgp_key_sequence_t keys;
    keys.keys.push_back(key);
    return write_transferable_keys(keys, &dst, armor);
}

bool
write_transferable_keys(pgp_key_sequence_t &keys, pgp_dest_t *dst, bool armor)
{
    std::unique_ptr<rnp::ArmoredDest> armdst;
    if (armor) {
        pgp_armored_msg_t msgtype = PGP_ARMORED_PUBLIC_KEY;
        if (!keys.keys.empty() && is_secret_key_pkt(keys.keys.front().key.tag)) {
            msgtype = PGP_ARMORED_SECRET_KEY;
        }
        armdst = std::unique_ptr<rnp::ArmoredDest>(new rnp::ArmoredDest(*dst, msgtype));
        dst = &armdst->dst();
    }

    for (auto &key : keys.keys) {
        /* main key */
        key.key.write(*dst);
        /* revocation and direct-key signatures */
        for (auto &sig : key.signatures) {
            sig.write(*dst);
        }
        /* user ids/attrs and signatures */
        for (auto &uid : key.userids) {
            uid.uid.write(*dst);
            for (auto &sig : uid.signatures) {
                sig.write(*dst);
            }
        }
        /* subkeys with signatures */
        for (auto &skey : key.subkeys) {
            skey.subkey.write(*dst);
            for (auto &sig : skey.signatures) {
                sig.write(*dst);
            }
        }
    }
    return !dst->werr;
}

bool
check_uid_valid(rnp_key_handle_t key, size_t idx, bool valid)
{
    rnp_uid_handle_t uid = NULL;
    if (rnp_key_get_uid_handle_at(key, idx, &uid)) {
        return false;
    }
    bool val = !valid;
    rnp_uid_is_valid(uid, &val);
    rnp_uid_handle_destroy(uid);
    return val == valid;
}

bool
check_uid_primary(rnp_key_handle_t key, size_t idx, bool primary)
{
    rnp_uid_handle_t uid = NULL;
    if (rnp_key_get_uid_handle_at(key, idx, &uid)) {
        return false;
    }
    bool prim = !primary;
    rnp_uid_is_primary(uid, &prim);
    rnp_uid_handle_destroy(uid);
    return prim == primary;
}

bool
check_key_valid(rnp_key_handle_t key, bool validity)
{
    bool valid = !validity;
    if (rnp_key_is_valid(key, &valid)) {
        return false;
    }
    return valid == validity;
}

bool
check_key_revoked(rnp_key_handle_t key, bool revoked)
{
    bool rev = !revoked;
    if (rnp_key_is_revoked(key, &rev)) {
        return false;
    }
    return rev == revoked;
}

bool
check_key_locked(rnp_key_handle_t key, bool locked)
{
    bool lock = !locked;
    if (rnp_key_is_locked(key, &lock)) {
        return false;
    }
    return lock == locked;
}

uint32_t
get_key_expiry(rnp_key_handle_t key)
{
    uint32_t expiry = (uint32_t) -1;
    rnp_key_get_expiration(key, &expiry);
    return expiry;
}

size_t
get_key_uids(rnp_key_handle_t key)
{
    size_t count = (size_t) -1;
    rnp_key_get_uid_count(key, &count);
    return count;
}

bool
check_sub_valid(rnp_key_handle_t key, size_t idx, bool validity)
{
    rnp_key_handle_t sub = NULL;
    if (rnp_key_get_subkey_at(key, idx, &sub)) {
        return false;
    }
    bool valid = !validity;
    rnp_key_is_valid(sub, &valid);
    rnp_key_handle_destroy(sub);
    return valid == validity;
}

bool
check_key_grip(rnp_key_handle_t key, const std::string &expected)
{
    char *grip = NULL;
    if (rnp_key_get_grip(key, &grip)) {
        return false;
    }
    bool res = !strcmp(grip, expected.c_str());
    rnp_buffer_destroy(grip);
    return res;
}

bool
check_key_fp(rnp_key_handle_t key, const std::string &expected)
{
    char *fp = NULL;
    if (rnp_key_get_fprint(key, &fp)) {
        return false;
    }
    bool res = !strcmp(fp, expected.c_str());
    rnp_buffer_destroy(fp);
    return res;
}

bool
check_key_revreason(rnp_key_handle_t key, const char *reason)
{
    char *rstr = NULL;
    if (rnp_key_get_revocation_reason(key, &rstr)) {
        return false;
    }
    bool res = !strcmp(rstr, reason);
    rnp_buffer_destroy(rstr);
    return res;
}

bool
check_has_key(rnp_ffi_t ffi, const std::string &id, bool secret, bool valid)
{
    rnp_key_handle_t key = NULL;
    switch (id.size()) {
    /* keyid with or without 0x */
    case 16:
    case 18:
        if (rnp_locate_key(ffi, "keyid", id.c_str(), &key) || !key) {
            return false;
        }
        break;
    /* v4 fingerprint with or without 0x */
    case 40:
    case 42:
    /* v5 fingerprint with or without 0x */
    case 64:
    case 66:
        if (rnp_locate_key(ffi, "fingerprint", id.c_str(), &key) || !key) {
            return false;
        }
        break;
    default:
        if (rnp_locate_key(ffi, "userid", id.c_str(), &key) || !key) {
            return false;
        }
        break;
    }
    bool res = true;
    if (secret && rnp_key_have_secret(key, &res)) {
        res = false;
    }
    res = res && check_key_valid(key, valid);
    rnp_key_handle_destroy(key);
    return res;
}

bool
check_sig_hash(rnp_signature_handle_t sig, const char *hash)
{
    char *sighash = NULL;
    if (rnp_signature_get_hash_alg(sig, &sighash)) {
        return false;
    }
    bool res = !strcmp(sighash, hash);
    rnp_buffer_destroy(sighash);
    return res;
}

bool
check_sig_type(rnp_signature_handle_t sig, const char *type)
{
    char *sigtype = NULL;
    if (rnp_signature_get_type(sig, &sigtype)) {
        return false;
    }
    bool res = !strcmp(sigtype, type);
    rnp_buffer_destroy(sigtype);
    return res;
}

bool
check_sig_revreason(rnp_signature_handle_t sig, const char *revcode, const char *revreason)
{
    char *sigcode = NULL;
    char *sigreason = NULL;
    if (rnp_signature_get_revocation_reason(sig, &sigcode, &sigreason)) {
        return false;
    }
    bool res = !strcmp(sigcode, revcode) && !strcmp(sigreason, revreason);
    rnp_buffer_destroy(sigcode);
    rnp_buffer_destroy(sigreason);
    return res;
}

rnp_key_handle_t
get_key_by_fp(rnp_ffi_t ffi, const char *fp)
{
    rnp_key_handle_t key = NULL;
    rnp_locate_key(ffi, "fingerprint", fp, &key);
    return key;
}

rnp_key_handle_t
get_key_by_uid(rnp_ffi_t ffi, const char *uid)
{
    rnp_key_handle_t key = NULL;
    rnp_locate_key(ffi, "userid", uid, &key);
    return key;
}

rnp_key_handle_t
bogus_key_handle(rnp_ffi_t ffi)
{
    return new rnp_key_handle_st(ffi);
}

bool
sm2_enabled()
{
    bool enabled = false;
    return !rnp_supports_feature(RNP_FEATURE_PK_ALG, "SM2", &enabled) && enabled;
}

bool
aead_eax_enabled()
{
    bool enabled = false;
    return !rnp_supports_feature(RNP_FEATURE_AEAD_ALG, "EAX", &enabled) && enabled;
}

bool
aead_ocb_enabled()
{
    bool enabled = false;
    return !rnp_supports_feature(RNP_FEATURE_AEAD_ALG, "OCB", &enabled) && enabled;
}

bool
aead_ocb_aes_only()
{
    return aead_ocb_enabled() && !strcmp(rnp_backend_string(), "OpenSSL");
}

bool
twofish_enabled()
{
    bool enabled = false;
    return !rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "Twofish", &enabled) && enabled;
}

bool
idea_enabled()
{
    bool enabled = false;
    return !rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "IDEA", &enabled) && enabled;
}

bool
brainpool_enabled()
{
    bool enabled = false;
    return !rnp_supports_feature(RNP_FEATURE_CURVE, "brainpoolP256r1", &enabled) && enabled;
}

bool
blowfish_enabled()
{
    bool enabled = false;
    return !rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "BLOWFISH", &enabled) && enabled;
}

bool
cast5_enabled()
{
    bool enabled = false;
    return !rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "CAST5", &enabled) && enabled;
}

bool
ripemd160_enabled()
{
    bool enabled = false;
    return !rnp_supports_feature(RNP_FEATURE_HASH_ALG, "RIPEMD160", &enabled) && enabled;
}

bool
test_load_gpg_check_key(rnp::KeyStore *pub, rnp::KeyStore *sec, const char *id)
{
    rnp::Key *key = rnp_tests_get_key_by_id(pub, id);
    if (!key) {
        return false;
    }
    if (!(key = rnp_tests_get_key_by_id(sec, id))) {
        return false;
    }
    pgp_password_provider_t pswd_prov(string_copy_password_callback, (void *) "password");
    return key->is_protected() && key->unlock(pswd_prov) && key->lock();
}
