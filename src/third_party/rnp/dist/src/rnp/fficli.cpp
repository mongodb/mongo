/*
 * Copyright (c) 2019-2021, 2023 [Ribose Inc](https://www.ribose.com).
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
#include "rnpcfg.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <iterator>
#include <cassert>
#include <ctype.h>
#ifdef _MSC_VER
#include "uniwin.h"
#else
#include <sys/param.h>
#include <unistd.h>
#endif

#ifndef _WIN32
#include <termios.h>
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif
#endif

#ifdef _WIN32
#include <crtdbg.h>
#endif

#include "fficli.h"
#include "str-utils.h"
#include "file-utils.h"
#include "time-utils.h"
#include "defaults.h"

#ifndef RNP_USE_STD_REGEX
#include <regex.h>
#else
#include <regex>
#endif

#ifdef HAVE_SYS_RESOURCE_H
/* When system resource consumption limit controls are available this
 * can be used to attempt to disable core dumps which may leak
 * sensitive data.
 *
 * Returns false if disabling core dumps failed, returns true if disabling
 * core dumps succeeded. errno will be set to the result from setrlimit in
 * the event of failure.
 */
static bool
disable_core_dumps(void)
{
    struct rlimit limit;
    int           error;

    errno = 0;
    memset(&limit, 0, sizeof(limit));
    error = setrlimit(RLIMIT_CORE, &limit);

    if (error == 0) {
        error = getrlimit(RLIMIT_CORE, &limit);
        if (error) {
            ERR_MSG("Warning - cannot turn off core dumps");
            return false;
        } else if (limit.rlim_cur == 0) {
            return true; // disabling core dumps ok
        } else {
            return false; // failed for some reason?
        }
    }
    return false;
}
#endif

#ifdef _WIN32
#include <windows.h>
#include <stdexcept>

static std::vector<std::string>
get_utf8_args()
{
    int       arg_nb;
    wchar_t **arg_w;

    arg_w = CommandLineToArgvW(GetCommandLineW(), &arg_nb);
    if (!arg_w) {
        throw std::runtime_error("CommandLineToArgvW failed");
    }

    try {
        std::vector<std::string> result;
        result.reserve(arg_nb);
        for (int i = 0; i < arg_nb; i++) {
            auto utf8 = wstr_to_utf8(arg_w[i]);
            result.push_back(utf8);
        }
        LocalFree(arg_w);
        return result;
    } catch (...) {
        LocalFree(arg_w);
        throw;
    }
}

void
rnp_win_clear_args(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (argv[i]) {
            free(argv[i]);
        }
    }
    delete argv;
}

bool
rnp_win_substitute_cmdline_args(int *argc, char ***argv)
{
    int    argc_utf8 = 0;
    char **argv_utf8_cstrs = NULL;
    try {
        auto argv_utf8_strings = get_utf8_args();
        argc_utf8 = argv_utf8_strings.size();
        *argc = argc_utf8;
        argv_utf8_cstrs = new (std::nothrow) char *[argc_utf8 + 1]();
        if (!argv_utf8_cstrs) {
            throw std::bad_alloc();
        }
        for (int i = 0; i < argc_utf8; i++) {
            auto arg_utf8 = strdup(argv_utf8_strings[i].c_str());
            if (!arg_utf8) {
                throw std::bad_alloc();
            }
            argv_utf8_cstrs[i] = arg_utf8;
        }
        /* argv must be terminated with NULL string */
        argv_utf8_cstrs[argc_utf8] = NULL;
    } catch (...) {
        if (argv_utf8_cstrs) {
            rnp_win_clear_args(argc_utf8, argv_utf8_cstrs);
        }
        throw;
    }
    *argc = argc_utf8;
    *argv = argv_utf8_cstrs;
    return true;
}
#endif

static bool
set_pass_fd(FILE **file, int passfd)
{
    if (!file) {
        return false;
    }
    *file = rnp_fdopen(passfd, "r");
    if (!*file) {
        ERR_MSG("Cannot open fd %d for reading", passfd);
        return false;
    }
    return true;
}

static char *
ptimestr(char *dest, size_t size, time_t t)
{
    struct tm tm = {};
    rnp_gmtime(t, tm);
    (void) snprintf(dest,
                    size,
                    "%s%04d-%02d-%02d",
                    rnp_y2k38_warning(t) ? ">=" : "",
                    tm.tm_year + 1900,
                    tm.tm_mon + 1,
                    tm.tm_mday);
    return dest;
}

static bool
cli_rnp_get_confirmation(const cli_rnp_t *rnp, const char *msg, ...)
{
    char    reply[10];
    va_list ap;

    while (true) {
        va_start(ap, msg);
        vfprintf(rnp->userio_out, msg, ap);
        va_end(ap);
        fprintf(rnp->userio_out, " (y/N) ");
        fflush(rnp->userio_out);

        if (fgets(reply, sizeof(reply), rnp->userio_in) == NULL) {
            return false;
        }

        rnp::strip_eol(reply);

        if (strlen(reply) > 0) {
            if (toupper(reply[0]) == 'Y') {
                return true;
            } else if (toupper(reply[0]) == 'N') {
                return false;
            }

            fprintf(rnp->userio_out, "Sorry, response '%s' not understood.\n", reply);
        } else {
            return false;
        }
    }

    return false;
}

static bool
rnp_ask_filename(const std::string &msg, std::string &res, cli_rnp_t &rnp)
{
    fprintf(rnp.userio_out, "%s", msg.c_str());
    fflush(rnp.userio_out);
    char        fname[128] = {0};
    std::string path;
    do {
        if (!fgets(fname, sizeof(fname), rnp.userio_in)) {
            return false;
        }
        path = path + std::string(fname);
        if (rnp::strip_eol(path)) {
            res = std::move(path);
            return true;
        }
        if (path.size() >= 2048) {
            fprintf(rnp.userio_out, "%s", "Too long filename, aborting.");
            fflush(rnp.userio_out);
            return false;
        }
    } while (1);
}

/** @brief checks whether file exists already and asks user for the new filename
 *  @param path output file name with path. May be an empty string, then user is asked for it.
 *  @param res resulting output path will be stored here.
 *  @param rnp initialized cli_rnp_t structure with additional data
 *  @return true on success, or false otherwise (user cancels the operation)
 **/

static bool
rnp_get_output_filename(const std::string &path, std::string &res, cli_rnp_t &rnp)
{
    std::string newpath = path;
    if (newpath.empty() &&
        !rnp_ask_filename("Please enter the output filename: ", newpath, rnp)) {
        return false;
    }

    while (true) {
        if (!rnp_file_exists(newpath.c_str())) {
            res = std::move(newpath);
            return true;
        }
        if (rnp.cfg().get_bool(CFG_OVERWRITE) ||
            cli_rnp_get_confirmation(
              &rnp,
              "File '%s' already exists. Would you like to overwrite it?",
              newpath.c_str())) {
            rnp_unlink(newpath.c_str());
            res = std::move(newpath);
            return true;
        }

        if (!rnp_ask_filename("Please enter the new filename: ", newpath, rnp)) {
            return false;
        }
        if (newpath.empty()) {
            return false;
        }
    }
}

static bool
stdin_getpass(const char *prompt, char *buffer, size_t size, cli_rnp_t &rnp)
{
#ifndef _WIN32
    struct termios saved_flags, noecho_flags;
    bool           restore_ttyflags = false;
#endif
    bool  ok = false;
    FILE *in = NULL;
    FILE *out = NULL;
    FILE *userio_in = rnp.userio_in ? rnp.userio_in : stdin;

    // validate args
    if (!buffer) {
        goto end;
    }
    // doesn't hurt
    *buffer = '\0';

    if (!rnp.cfg().get_bool(CFG_NOTTY)) {
#ifndef _WIN32
        in = fopen("/dev/tty", "w+ce");
#endif
        out = in;
    }

    if (!in) {
        in = userio_in;
        out = rnp.userio_out ? rnp.userio_out : stdout;
    }

    // TODO: Implement alternative for hiding password entry on Windows
    // TODO: avoid duplicate termios code with pass-provider.cpp
#ifndef _WIN32
    // save the original termios
    if (tcgetattr(fileno(in), &saved_flags) == 0) {
        noecho_flags = saved_flags;
        // disable echo in the local modes
        noecho_flags.c_lflag = (noecho_flags.c_lflag & ~ECHO) | ECHONL | ISIG;
        restore_ttyflags = (tcsetattr(fileno(in), TCSANOW, &noecho_flags) == 0);
    }
#endif
    if (prompt) {
        fputs(prompt, out);
    }
    if (fgets(buffer, size, in) == NULL) {
        goto end;
    }

    rnp::strip_eol(buffer);
    ok = true;
end:
#ifndef _WIN32
    if (restore_ttyflags) {
        tcsetattr(fileno(in), TCSAFLUSH, &saved_flags);
    }
#endif
    if (in && (in != userio_in)) {
        fclose(in);
    }
    return ok;
}

static bool
ffi_pass_callback_stdin(rnp_ffi_t        ffi,
                        void *           app_ctx,
                        rnp_key_handle_t key,
                        const char *     pgp_context,
                        char             buf[],
                        size_t           buf_len)
{
    char *     keyid = NULL;
    char       target[64] = {0};
    char       prompt[128] = {0};
    char *     buffer = NULL;
    bool       ok = false;
    bool       protect = false;
    bool       add_subkey = false;
    bool       decrypt_symmetric = false;
    bool       encrypt_symmetric = false;
    bool       is_primary = false;
    cli_rnp_t *rnp = static_cast<cli_rnp_t *>(app_ctx);

    if (!ffi || !pgp_context) {
        goto done;
    }

    if (!strcmp(pgp_context, "protect")) {
        protect = true;
    } else if (!strcmp(pgp_context, "add subkey")) {
        add_subkey = true;
    } else if (!strcmp(pgp_context, "decrypt (symmetric)")) {
        decrypt_symmetric = true;
    } else if (!strcmp(pgp_context, "encrypt (symmetric)")) {
        encrypt_symmetric = true;
    }

    if (!decrypt_symmetric && !encrypt_symmetric) {
        rnp_key_get_keyid(key, &keyid);
        snprintf(target, sizeof(target), "key 0x%s", keyid);
        rnp_buffer_destroy(keyid);
        (void) rnp_key_is_primary(key, &is_primary);
    }

    if ((protect || add_subkey) && rnp->reuse_password_for_subkey && !is_primary) {
        char *primary_fprint = NULL;
        if (rnp_key_get_primary_fprint(key, &primary_fprint) == RNP_SUCCESS &&
            !rnp->reuse_primary_fprint.empty() &&
            rnp->reuse_primary_fprint == primary_fprint) {
            strncpy(buf, rnp->reused_password, buf_len);
            ok = true;
        }

        rnp->reuse_password_for_subkey--;
        if (!rnp->reuse_password_for_subkey) {
            rnp_buffer_clear(rnp->reused_password, strnlen(rnp->reused_password, buf_len));
            free(rnp->reused_password);
            rnp->reused_password = NULL;
            rnp_buffer_destroy(primary_fprint);
        }
        if (ok)
            return true;
    }

    buffer = (char *) calloc(1, buf_len);
    if (!buffer) {
        return false;
    }
start:
    if (decrypt_symmetric) {
        snprintf(prompt, sizeof(prompt), "Enter password to decrypt data: ");
    } else if (encrypt_symmetric) {
        snprintf(prompt, sizeof(prompt), "Enter password to encrypt data: ");
    } else {
        snprintf(prompt, sizeof(prompt), "Enter password for %s to %s: ", target, pgp_context);
    }

    if (!stdin_getpass(prompt, buf, buf_len, *rnp)) {
        goto done;
    }
    if (protect || encrypt_symmetric) {
        if (protect) {
            snprintf(prompt, sizeof(prompt), "Repeat password for %s: ", target);
        } else {
            snprintf(prompt, sizeof(prompt), "Repeat password: ");
        }

        if (!stdin_getpass(prompt, buffer, buf_len, *rnp)) {
            goto done;
        }
        if (strcmp(buf, buffer) != 0) {
            fputs("\nPasswords do not match!", rnp->userio_out);
            // currently will loop forever
            goto start;
        }
        if (strnlen(buf, buf_len) == 0 && !rnp->cfg().get_bool(CFG_FORCE)) {
            if (!cli_rnp_get_confirmation(
                  rnp, "Password is empty. The key will be left unprotected. Are you sure?")) {
                goto start;
            }
        }
    }
    if ((protect || add_subkey) && is_primary) {
        if (cli_rnp_get_confirmation(
              rnp, "Would you like to use the same password to protect subkey(s)?")) {
            char *primary_fprint = NULL;
            rnp_key_get_subkey_count(key, &(rnp->reuse_password_for_subkey));
            rnp_key_get_fprint(key, &primary_fprint);
            rnp->reuse_primary_fprint = primary_fprint;
            rnp->reused_password = strdup(buf);
            rnp_buffer_destroy(primary_fprint);
        }
    }
    ok = true;
done:
    fputs("", rnp->userio_out);
    rnp_buffer_clear(buffer, buf_len);
    free(buffer);
    return ok;
}

static bool
ffi_pass_callback_file(rnp_ffi_t        ffi,
                       void *           app_ctx,
                       rnp_key_handle_t key,
                       const char *     pgp_context,
                       char             buf[],
                       size_t           buf_len)
{
    if (!app_ctx || !buf || !buf_len) {
        return false;
    }

    FILE *fp = (FILE *) app_ctx;
    if (!fgets(buf, buf_len, fp)) {
        return false;
    }
    rnp::strip_eol(buf);
    return true;
}

static bool
ffi_pass_callback_string(rnp_ffi_t        ffi,
                         void *           app_ctx,
                         rnp_key_handle_t key,
                         const char *     pgp_context,
                         char             buf[],
                         size_t           buf_len)
{
    if (!app_ctx || !buf || !buf_len) {
        return false;
    }

    const char *pswd = (const char *) app_ctx;
    if (strlen(pswd) >= buf_len) {
        return false;
    }

    strncpy(buf, pswd, buf_len);
    return true;
}

static void
ffi_key_callback(rnp_ffi_t   ffi,
                 void *      app_ctx,
                 const char *identifier_type,
                 const char *identifier,
                 bool        secret)
{
    cli_rnp_t *rnp = static_cast<cli_rnp_t *>(app_ctx);

    if (rnp::str_case_eq(identifier_type, "keyid") &&
        rnp::str_case_eq(identifier, "0000000000000000")) {
        if (rnp->hidden_msg) {
            return;
        }
        ERR_MSG("This message has hidden recipient. Will attempt to use all secret keys for "
                "decryption.");
        rnp->hidden_msg = true;
    }
}

#ifdef _WIN32
static void
rnpffiInvalidParameterHandler(const wchar_t *expression,
                              const wchar_t *function,
                              const wchar_t *file,
                              unsigned int   line,
                              uintptr_t      pReserved)
{
    // do nothing as within release CRT all params are NULL
}
#endif

cli_rnp_t::~cli_rnp_t()
{
    end();
#ifdef _WIN32
    if (subst_argv) {
        rnp_win_clear_args(subst_argc, subst_argv);
    }
#endif
}

int
cli_rnp_t::ret_code(bool success)
{
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}

#ifdef _WIN32
void
cli_rnp_t::substitute_args(int *argc, char ***argv)
{
    rnp_win_substitute_cmdline_args(argc, argv);
    subst_argc = *argc;
    subst_argv = *argv;
}
#endif

bool
cli_rnp_t::init(const rnp_cfg &cfg)
{
    cfg_.copy(cfg);

    /* Configure user's io streams. */
    if (!cfg_.get_bool(CFG_NOTTY)) {
        userio_in = (isatty(fileno(stdin)) ? stdin : fopen("/dev/tty", "r"));
        userio_in = (userio_in ? userio_in : stdin);
        userio_out = (isatty(fileno(stdout)) ? stdout : fopen("/dev/tty", "a+"));
        userio_out = (userio_out ? userio_out : stdout);
    } else {
        userio_in = stdin;
        userio_out = stdout;
    }

#ifndef _WIN32
    /* If system resource constraints are in effect then attempt to
     * disable core dumps.
     */
    bool coredumps = true;
    if (!cfg_.get_bool(CFG_COREDUMPS)) {
#ifdef HAVE_SYS_RESOURCE_H
        coredumps = !disable_core_dumps();
#endif
    }

    if (coredumps) {
        ERR_MSG("warning: core dumps may be enabled, sensitive data may be leaked to disk");
    }
#endif

#ifdef _WIN32
    /* Setup invalid parameter handler for Windows */
    _invalid_parameter_handler handler = rnpffiInvalidParameterHandler;
    _set_invalid_parameter_handler(handler);
    _CrtSetReportMode(_CRT_ASSERT, 0);
#endif

    /* Configure the results stream. */
    // TODO: UTF8?
    const std::string &ress = cfg_.get_str(CFG_IO_RESS);
    if (ress.empty() || (ress == "<stderr>")) {
        resfp = stderr;
    } else if (ress == "<stdout>") {
        resfp = stdout;
    } else if (!(resfp = rnp_fopen(ress.c_str(), "w"))) {
        ERR_MSG("Cannot open results %s for writing", ress.c_str());
        return false;
    }

    bool              res = false;
    const std::string pformat = pubformat();
    const std::string sformat = secformat();
    if (pformat.empty() || sformat.empty()) {
        ERR_MSG("Unknown public or secret keyring format");
        return false;
    }
    if (rnp_ffi_create(&ffi, pformat.c_str(), sformat.c_str())) {
        ERR_MSG("Failed to initialize FFI");
        return false;
    }

    if (cfg_.has(CFG_ALLOW_SHA1)) {
        auto     now = time(NULL);
        uint64_t from = 0;
        uint32_t level = 0;
        rnp_get_security_rule(ffi, RNP_FEATURE_HASH_ALG, "SHA1", now, NULL, &from, &level);
        rnp_add_security_rule(ffi,
                              RNP_FEATURE_HASH_ALG,
                              "SHA1",
                              RNP_SECURITY_OVERRIDE | RNP_SECURITY_VERIFY_KEY,
                              from,
                              RNP_SECURITY_DEFAULT);
    }

    if (cfg_.has(CFG_ALLOW_OLD_CIPHERS)) {
        auto     now = time(NULL);
        uint64_t from = 0;
        uint32_t level = 0;
        rnp_get_security_rule(ffi, RNP_FEATURE_SYMM_ALG, "CAST5", now, NULL, &from, &level);
        rnp_add_security_rule(ffi,
                              RNP_FEATURE_SYMM_ALG,
                              "CAST5",
                              RNP_SECURITY_OVERRIDE,
                              from,
                              RNP_SECURITY_DEFAULT);
        rnp_get_security_rule(
          ffi, RNP_FEATURE_SYMM_ALG, "TRIPLEDES", now, NULL, &from, &level);
        rnp_add_security_rule(ffi,
                              RNP_FEATURE_SYMM_ALG,
                              "TRIPLEDES",
                              RNP_SECURITY_OVERRIDE,
                              from,
                              RNP_SECURITY_DEFAULT);
        rnp_get_security_rule(ffi, RNP_FEATURE_SYMM_ALG, "IDEA", now, NULL, &from, &level);
        rnp_add_security_rule(ffi,
                              RNP_FEATURE_SYMM_ALG,
                              "IDEA",
                              RNP_SECURITY_OVERRIDE,
                              from,
                              RNP_SECURITY_DEFAULT);
        rnp_get_security_rule(ffi, RNP_FEATURE_SYMM_ALG, "BLOWFISH", now, NULL, &from, &level);
        rnp_add_security_rule(ffi,
                              RNP_FEATURE_SYMM_ALG,
                              "BLOWFISH",
                              RNP_SECURITY_OVERRIDE,
                              from,
                              RNP_SECURITY_DEFAULT);
    }

    // by default use stdin password provider
    if (rnp_ffi_set_pass_provider(ffi, ffi_pass_callback_stdin, this)) {
        goto done;
    }

    // set key provider, currently for informational purposes only
    if (rnp_ffi_set_key_provider(ffi, ffi_key_callback, this)) {
        goto done;
    }

    // setup file/pipe password input if requested
    if (cfg_.get_int(CFG_PASSFD, -1) >= 0) {
        if (!set_pass_fd(&passfp, cfg_.get_int(CFG_PASSFD))) {
            goto done;
        }
        if (rnp_ffi_set_pass_provider(ffi, ffi_pass_callback_file, passfp)) {
            goto done;
        }
    }
    // setup current time if requested
    if (cfg_.has(CFG_CURTIME)) {
        rnp_set_timestamp(ffi, cfg_.time());
    }
    pswdtries = MAX_PASSWORD_ATTEMPTS;
    res = true;
done:
    if (!res) {
        rnp_ffi_destroy(ffi);
        ffi = NULL;
    }
    return res;
}

void
cli_rnp_t::end()
{
    if (passfp) {
        fclose(passfp);
        passfp = NULL;
    }
    if (resfp && (resfp != stderr) && (resfp != stdout)) {
        fclose(resfp);
        resfp = NULL;
    }
    if (userio_in && userio_in != stdin) {
        fclose(userio_in);
    }
    userio_in = NULL;
    if (userio_out && userio_out != stdout) {
        fclose(userio_out);
    }
    userio_out = NULL;
    rnp_ffi_destroy(ffi);
    ffi = NULL;
    cfg_.clear();
    reuse_primary_fprint.clear();
    if (reused_password) {
        rnp_buffer_clear(reused_password, strlen(reused_password));
        free(reused_password);
        reused_password = NULL;
    }
    reuse_password_for_subkey = 0;
}

bool
cli_rnp_t::load_keyring(bool secret)
{
    const std::string &path = secret ? secpath() : pubpath();
    bool               dir = secret && (secformat() == RNP_KEYSTORE_G10);
    if (!rnp::path::exists(path, dir)) {
        return true;
    }

    rnp_input_t keyin = NULL;
    if (rnp_input_from_path(&keyin, path.c_str())) {
        ERR_MSG("Warning: failed to open keyring at path '%s' for reading.", path.c_str());
        return true;
    }

    const char * format = secret ? secformat().c_str() : pubformat().c_str();
    uint32_t     flags = secret ? RNP_LOAD_SAVE_SECRET_KEYS : RNP_LOAD_SAVE_PUBLIC_KEYS;
    rnp_result_t ret = rnp_load_keys(ffi, format, keyin, flags);
    if (ret) {
        ERR_MSG("Error: failed to load keyring from '%s'", path.c_str());
    }
    rnp_input_destroy(keyin);

    if (ret) {
        return false;
    }

    size_t keycount = 0;
    if (secret) {
        (void) rnp_get_secret_key_count(ffi, &keycount);
    } else {
        (void) rnp_get_public_key_count(ffi, &keycount);
    }
    if (!keycount) {
        ERR_MSG("Warning: no keys were loaded from the keyring '%s'.", path.c_str());
    }
    return true;
}

bool
cli_rnp_t::load_keyrings(bool loadsecret)
{
    /* Read public keys */
    if (rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC)) {
        ERR_MSG("failed to clear public keyring");
        return false;
    }

    if (!load_keyring(false)) {
        return false;
    }

    /* Only read secret keys if we need to */
    if (loadsecret) {
        if (rnp_unload_keys(ffi, RNP_KEY_UNLOAD_SECRET)) {
            ERR_MSG("failed to clear secret keyring");
            return false;
        }

        if (!load_keyring(true)) {
            return false;
        }
    }
    if (defkey().empty()) {
        set_defkey();
    }
    return true;
}

void
cli_rnp_t::set_defkey()
{
    cfg_.unset(CFG_KR_DEF_KEY);

    rnpffi::FFI ffiobj(ffi, false);
    auto        it = ffiobj.iterator_create("fingerprint");
    if (!it) {
        ERR_MSG("failed to create key iterator");
        return;
    }

    std::string fp;
    while (it->next(fp)) {
        auto key = ffiobj.locate_key("fingerprint", fp);
        if (!key) {
            ERR_MSG("failed to locate key %s", fp.c_str());
            continue;
        }
        if (!key->is_primary()) {
            continue;
        }
        bool is_secret = key->secret();
        if (!cfg_.has(CFG_KR_DEF_KEY) || is_secret) {
            cfg_.set_str(CFG_KR_DEF_KEY, fp);
            /* if we have secret primary key then use it as default */
            if (is_secret) {
                return;
            }
        }
    }
}

bool
cli_rnp_t::is_cv25519_subkey(rnpffi::Key &key)
{
    try {
        if (key.is_primary()) {
            return false;
        }
        if (strcmp(key.alg().c_str(), RNP_ALGNAME_ECDH)) {
            return false;
        }
        return !strcmp(key.curve().c_str(), "Curve25519");
    } catch (const rnpffi::ffi_exception &e) {
        ERR_MSG("FFI call error: %s.", e.func());
        return false;
    }
}

bool
cli_rnp_t::get_protection(rnpffi::Key &key,
                          std::string &hash,
                          std::string &cipher,
                          size_t &     iterations)
{
    try {
        if (!key.is_protected()) {
            hash = "";
            cipher = "";
            iterations = 0;
            return true;
        }
        hash = key.protection_hash();
        cipher = key.protection_cipher();
        iterations = key.protection_iterations();
        return true;
    } catch (const rnpffi::ffi_exception &e) {
        ERR_MSG("FFI call error: %s", e.func());
        return false;
    }
}

bool
cli_rnp_t::check_cv25519_bits(rnpffi::Key &key, rnpffi::String &prot_password, bool &tweaked)
{
    /* unlock key first to check whether bits are tweaked */
    if (prot_password.c_str() && !key.unlock(prot_password.str())) {
        ERR_MSG("Error: failed to unlock key. Did you specify valid password?");
        return false;
    }
    bool res = false;
    try {
        tweaked = key.is_25519_bits_tweaked();
        res = true;
    } catch (...) {
        ERR_MSG("Error: failed to check whether key's bits are tweaked.");
    }
    if (prot_password.c_str()) {
        key.lock();
    }
    return res;
}

bool
cli_rnp_t::fix_cv25519_subkey(const std::string &str, bool checkonly)
{
    size_t keys = 0;
    auto   key = key_matching(str, CLI_SEARCH_SECRET | CLI_SEARCH_SUBKEYS, &keys);
    if (!keys) {
        ERR_MSG("Secret keys matching '%s' not found.", str.c_str());
        return false;
    }
    if (keys > 1) {
        ERR_MSG(
          "Ambiguous input: too many keys found for '%s'. Did you use keyid or fingerprint?",
          str.c_str());
        return false;
    }
    cli_rnp_print_key_info(userio_out, ffi, key->handle(), true, false);
    if (!is_cv25519_subkey(*key)) {
        ERR_MSG("Error: specified key is not Curve25519 ECDH subkey.");
        return false;
    }

    std::string prot_hash;
    std::string prot_cipher;
    size_t      prot_iterations;
    if (!get_protection(*key, prot_hash, prot_cipher, prot_iterations)) {
        return false;
    }

    rnpffi::String prot_password(true);
    rnpffi::FFI    ffiobj(ffi, false);
    if (!prot_hash.empty() && (!ffiobj.request_password(*key, "unprotect", prot_password) ||
                               !prot_password.c_str())) {
        ERR_MSG("Error: failed to obtain protection password.");
        return false;
    }

    bool tweaked = false;
    if (!check_cv25519_bits(*key, prot_password, tweaked)) {
        return false;
    }

    if (checkonly) {
        fprintf(userio_out,
                tweaked ? "Cv25519 key bits are set correctly and do not require fixing.\n" :
                          "Warning: Cv25519 key bits need fixing.\n");
        return tweaked;
    }

    if (tweaked) {
        ERR_MSG("Warning: key's bits are fixed already, no action is required.");
        return true;
    }

    /* now unprotect so we can tweak bits */
    if (!prot_hash.empty()) {
        if (!key->unprotect(prot_password.str())) {
            ERR_MSG("Error: failed to unprotect key. Did you specify valid password?");
            return false;
        }
        if (!key->unlock()) {
            ERR_MSG("Error: failed to unlock key.");
            return false;
        }
    }

    /* tweak key bits and protect back */
    if (!key->do_25519_bits_tweak()) {
        ERR_MSG("Error: failed to tweak key's bits.");
        return false;
    }

    if (!prot_hash.empty() &&
        !key->protect(prot_password.str(), prot_cipher, prot_hash, prot_iterations)) {
        ERR_MSG("Error: failed to protect key back.");
        return false;
    }

    return cli_rnp_save_keyrings(this);
}

bool
cli_rnp_t::set_key_expire(const std::string &key)
{
    std::vector<rnp_key_handle_t> keys;
    if (!keys_matching(keys, key, CLI_SEARCH_SECRET | CLI_SEARCH_SUBKEYS)) {
        ERR_MSG("Secret keys matching '%s' not found.", key.c_str());
        return false;
    }
    bool     res = false;
    uint32_t expiration = 0;
    if (keys.size() > 1) {
        ERR_MSG("Ambiguous input: too many keys found for '%s'.", key.c_str());
        goto done;
    }
    if (!cfg().get_expiration(CFG_SET_KEY_EXPIRE, expiration) ||
        rnp_key_set_expiration(keys[0], expiration)) {
        ERR_MSG("Failed to set key expiration.");
        goto done;
    }
    res = cli_rnp_save_keyrings(this);
done:
    if (res) {
        cli_rnp_print_key_info(stdout, ffi, keys[0], true, false);
    }
    clear_key_handles(keys);
    return res;
}

bool
cli_rnp_t::add_new_subkey(const std::string &key)
{
    rnp_cfg &lcfg = cfg();
    if (!cli_rnp_set_generate_params(lcfg, true)) {
        ERR_MSG("Subkey generation setup failed.");
        return false;
    }
    std::vector<rnp_key_handle_t> keys;
    if (!keys_matching(keys, key, CLI_SEARCH_SECRET)) {
        ERR_MSG("Secret keys matching '%s' not found.", key.c_str());
        return false;
    }
    bool              res = false;
    rnp_op_generate_t genkey = NULL;
    rnp_key_handle_t  subkey = NULL;
    char *            password = NULL;

    if (keys.size() > 1) {
        ERR_MSG("Ambiguous input: too many keys found for '%s'.", key.c_str());
        goto done;
    }
    if (rnp_op_generate_subkey_create(
          &genkey, ffi, keys[0], cfg().get_cstr(CFG_KG_SUBKEY_ALG))) {
        ERR_MSG("Failed to initialize subkey generation.");
        goto done;
    }
    if (cfg().has(CFG_KG_SUBKEY_BITS) &&
        rnp_op_generate_set_bits(genkey, cfg().get_int(CFG_KG_SUBKEY_BITS))) {
        ERR_MSG("Failed to set subkey bits.");
        goto done;
    }
    if (cfg().has(CFG_KG_SUBKEY_CURVE) &&
        rnp_op_generate_set_curve(genkey, cfg().get_cstr(CFG_KG_SUBKEY_CURVE))) {
        ERR_MSG("Failed to set subkey curve.");
        goto done;
    }
    if (cfg().has(CFG_KG_SUBKEY_EXPIRATION)) {
        uint32_t expiration = 0;
        if (!cfg().get_expiration(CFG_KG_SUBKEY_EXPIRATION, expiration) ||
            rnp_op_generate_set_expiration(genkey, expiration)) {
            ERR_MSG("Failed to set subkey expiration.");
            goto done;
        }
    }
    // TODO : set DSA qbits
    if (rnp_op_generate_set_hash(genkey, cfg().get_cstr(CFG_KG_HASH))) {
        ERR_MSG("Failed to set hash algorithm.");
        goto done;
    }
    if (rnp_op_generate_execute(genkey) || rnp_op_generate_get_key(genkey, &subkey)) {
        ERR_MSG("Subkey generation failed.");
        goto done;
    }
    if (rnp_request_password(ffi, subkey, "protect", &password)) {
        ERR_MSG("Failed to obtain protection password.");
        goto done;
    }
    if (*password) {
        rnp_result_t ret = rnp_key_protect(subkey,
                                           password,
                                           cfg().get_cstr(CFG_KG_PROT_ALG),
                                           NULL,
                                           cfg().get_cstr(CFG_KG_PROT_HASH),
                                           cfg().get_int(CFG_KG_PROT_ITERATIONS));
        rnp_buffer_clear(password, strlen(password) + 1);
        rnp_buffer_destroy(password);
        if (ret) {
            ERR_MSG("Failed to protect key.");
            goto done;
        }
    } else {
        rnp_buffer_destroy(password);
    }
    res = cli_rnp_save_keyrings(this);
done:
    if (res) {
        cli_rnp_print_key_info(stdout, ffi, keys[0], true, false);
        if (subkey) {
            cli_rnp_print_key_info(stdout, ffi, subkey, true, false);
        }
    }
    clear_key_handles(keys);
    rnp_op_generate_destroy(genkey);
    rnp_key_handle_destroy(subkey);
    return res;
}

bool
cli_rnp_t::edit_key(const std::string &key)
{
    int edit_options = 0;

    if (cfg().get_bool(CFG_CHK_25519_BITS)) {
        edit_options++;
    }
    if (cfg().get_bool(CFG_FIX_25519_BITS)) {
        edit_options++;
    }
    if (cfg().get_bool(CFG_ADD_SUBKEY)) {
        edit_options++;
    }
    if (cfg().has(CFG_SET_KEY_EXPIRE)) {
        edit_options++;
    }

    if (!edit_options) {
        ERR_MSG("You should specify one of the editing options for --edit-key.");
        return false;
    }
    if (edit_options > 1) {
        ERR_MSG("Only one key edit option can be executed at a time.");
        return false;
    }

    if (cfg().get_bool(CFG_CHK_25519_BITS)) {
        return fix_cv25519_subkey(key, true);
    }
    if (cfg().get_bool(CFG_FIX_25519_BITS)) {
        return fix_cv25519_subkey(key, false);
    }

    if (cfg().get_bool(CFG_ADD_SUBKEY)) {
        return add_new_subkey(key);
    }

    if (cfg().has(CFG_SET_KEY_EXPIRE)) {
        return set_key_expire(key);
    }

    return false;
}

const char *
json_obj_get_str(json_object *obj, const char *key)
{
    json_object *fld = NULL;
    if (!json_object_object_get_ex(obj, key, &fld)) {
        return NULL;
    }
    return json_object_get_string(fld);
}

static char *
cli_key_usage_str(rnp_key_handle_t key, char *buf)
{
    char *orig = buf;
    bool  allow = false;

    if (!rnp_key_allows_usage(key, "encrypt", &allow) && allow) {
        *buf++ = 'E';
    }
    allow = false;
    if (!rnp_key_allows_usage(key, "sign", &allow) && allow) {
        *buf++ = 'S';
    }
    allow = false;
    if (!rnp_key_allows_usage(key, "certify", &allow) && allow) {
        *buf++ = 'C';
    }
    allow = false;
    if (!rnp_key_allows_usage(key, "authenticate", &allow) && allow) {
        *buf++ = 'A';
    }
    *buf = '\0';
    return orig;
}

std::string
cli_rnp_escape_string(const std::string &src)
{
    static const int   SPECIAL_CHARS_COUNT = 0x20;
    static const char *escape_map[SPECIAL_CHARS_COUNT + 1] = {
      "\\x00", "\\x01", "\\x02", "\\x03", "\\x04", "\\x05", "\\x06", "\\x07",
      "\\b",   "\\x09", "\\n",   "\\v",   "\\f",   "\\r",   "\\x0e", "\\x0f",
      "\\x10", "\\x11", "\\x12", "\\x13", "\\x14", "\\x15", "\\x16", "\\x17",
      "\\x18", "\\x19", "\\x1a", "\\x1b", "\\x1c", "\\x1d", "\\x1e", "\\x1f",
      "\\x20" // space should not be auto-replaced
    };
    std::string result;
    // we want to replace leading and trailing spaces with escape codes to make them visible
    auto        original_len = src.length();
    std::string rtrimmed = src;
    bool        leading_space = true;
    rtrimmed.erase(rtrimmed.find_last_not_of(0x20) + 1);
    result.reserve(original_len);
    for (char const &c : rtrimmed) {
        leading_space &= c == 0x20;
        if (leading_space || (c >= 0 && c < SPECIAL_CHARS_COUNT)) {
            result.append(escape_map[(int) c]);
        } else {
            result.push_back(c);
        }
    }
    // printing trailing spaces
    for (auto pos = rtrimmed.length(); pos < original_len; pos++) {
        result.append(escape_map[0x20]);
    }
    return result;
}

static const std::string alg_aliases[] = {
  "3DES",         "TRIPLEDES",   "3-DES",        "TRIPLEDES",   "CAST-5",       "CAST5",
  "AES",          "AES128",      "AES-128",      "AES128",      "AES-192",      "AES192",
  "AES-256",      "AES256",      "CAMELLIA-128", "CAMELLIA128", "CAMELLIA-192", "CAMELLIA192",
  "CAMELLIA-256", "CAMELLIA256", "SHA",          "SHA1",        "SHA-1",        "SHA1",
  "SHA-224",      "SHA224",      "SHA-256",      "SHA256",      "SHA-384",      "SHA384",
  "SHA-512",      "SHA512",      "RIPEMD-160",   "RIPEMD160"};

const std::string
cli_rnp_alg_to_ffi(const std::string &alg)
{
    size_t count = sizeof(alg_aliases) / sizeof(alg_aliases[0]);
    assert((count % 2) == 0);
    for (size_t idx = 0; idx < count; idx += 2) {
        if (rnp::str_case_eq(alg, alg_aliases[idx])) {
            return alg_aliases[idx + 1];
        }
    }
    return alg;
}

bool
cli_rnp_set_hash(rnp_cfg &cfg, const std::string &hash)
{
    bool  supported = false;
    auto &alg = cli_rnp_alg_to_ffi(hash);
    if (rnp_supports_feature(RNP_FEATURE_HASH_ALG, alg.c_str(), &supported) || !supported) {
        ERR_MSG("Unsupported hash algorithm: %s", hash.c_str());
        return false;
    }
    cfg.set_str(CFG_HASH, alg);
    return true;
}

bool
cli_rnp_set_cipher(rnp_cfg &cfg, const std::string &cipher)
{
    bool  supported = false;
    auto &alg = cli_rnp_alg_to_ffi(cipher);
    if (rnp_supports_feature(RNP_FEATURE_SYMM_ALG, alg.c_str(), &supported) || !supported) {
        ERR_MSG("Unsupported encryption algorithm: %s", cipher.c_str());
        return false;
    }
    cfg.set_str(CFG_CIPHER, alg);
    return true;
}

#ifndef RNP_USE_STD_REGEX
static std::string
cli_rnp_unescape_for_regcomp(const std::string &src)
{
    std::string result;
    result.reserve(src.length());
    regex_t    r = {};
    regmatch_t matches[1];
    if (regcomp(&r, "\\\\x[0-9a-f]([0-9a-f])?", REG_EXTENDED | REG_ICASE) != 0)
        return src;

    int offset = 0;
    while (regexec(&r, src.c_str() + offset, 1, matches, 0) == 0) {
        result.append(src, offset, matches[0].rm_so);
        int         hexoff = matches[0].rm_so + 2;
        std::string hex;
        hex.push_back(src[offset + hexoff]);
        if (hexoff + 1 < matches[0].rm_eo) {
            hex.push_back(src[offset + hexoff + 1]);
        }
        char decoded = stoi(hex, 0, 16);
        if ((decoded >= 0x7B && decoded <= 0x7D) || (decoded >= 0x24 && decoded <= 0x2E) ||
            decoded == 0x5C || decoded == 0x5E) {
            result.push_back('\\');
            result.push_back(decoded);
        } else if ((decoded == '[' || decoded == ']') &&
                   /* not enclosed in [] */ (result.empty() || result.back() != '[')) {
            result.push_back('[');
            result.push_back(decoded);
            result.push_back(']');
        } else {
            result.push_back(decoded);
        }
        offset += matches[0].rm_eo;
    }

    result.append(src.begin() + offset, src.end());

    return result;
}
#endif

/* Convert key algorithm constant to one displayed to the user */
static const char *
cli_rnp_normalize_key_alg(const char *alg)
{
    if (!strcmp(alg, RNP_ALGNAME_EDDSA)) {
        return "EdDSA";
    }
    if (!strcmp(alg, RNP_ALGNAME_ELGAMAL)) {
        return "ElGamal";
    }
    return alg;
}

static void
cli_rnp_print_sig_info(FILE *fp, rnp_ffi_t ffi, rnp_signature_handle_t sig)
{
    uint32_t creation = 0;
    (void) rnp_signature_get_creation(sig, &creation);

    char *keyfp = NULL;
    char *keyid = NULL;
    (void) rnp_signature_get_key_fprint(sig, &keyfp);
    (void) rnp_signature_get_keyid(sig, &keyid);

    char *           signer_uid = NULL;
    rnp_key_handle_t signer = NULL;
    if (keyfp) {
        /* Fingerprint lookup is faster */
        (void) rnp_locate_key(ffi, "fingerprint", keyfp, &signer);
    } else if (keyid) {
        (void) rnp_locate_key(ffi, "keyid", keyid, &signer);
    }
    if (signer) {
        /* signer primary uid */
        (void) rnp_key_get_primary_uid(signer, &signer_uid);
    }

    /* signer key id */
    fprintf(fp, "sig           %s ", keyid ? rnp::lowercase(keyid) : "[no key id]");
    /* signature creation time */
    char buf[64] = {0};
    fprintf(fp, "%s", ptimestr(buf, sizeof(buf), creation));
    /* signer's userid */
    fprintf(fp, " %s", signer_uid ? signer_uid : "[unknown]");
    /* signature validity */
    const char * valmsg = NULL;
    rnp_result_t validity = rnp_signature_is_valid(sig, 0);
    switch (validity) {
    case RNP_SUCCESS:
        valmsg = "";
        break;
    case RNP_ERROR_SIGNATURE_EXPIRED:
        valmsg = " [expired]";
        break;
    case RNP_ERROR_SIGNATURE_INVALID:
        valmsg = " [invalid]";
        break;
    default:
        valmsg = " [unverified]";
    }
    fprintf(fp, "%s\n", valmsg);

    (void) rnp_key_handle_destroy(signer);
    rnp_buffer_destroy(keyid);
    rnp_buffer_destroy(keyfp);
    rnp_buffer_destroy(signer_uid);
}

void
cli_rnp_print_key_info(FILE *fp, rnp_ffi_t ffi, rnp_key_handle_t key, bool psecret, bool psigs)
{
    char        buf[64] = {0};
    const char *header = NULL;
    bool        secret = false;
    bool        primary = false;

    /* header */
    if (rnp_key_have_secret(key, &secret) || rnp_key_is_primary(key, &primary)) {
        fprintf(fp, "Key error.\n");
        return;
    }

    if (psecret && secret) {
        header = primary ? "sec" : "ssb";
    } else {
        header = primary ? "pub" : "sub";
    }
    if (primary) {
        fprintf(fp, "\n");
    }
    fprintf(fp, "%s   ", header);

    /* key bits */
    uint32_t bits = 0;
    rnp_key_get_bits(key, &bits);
    fprintf(fp, "%d/", (int) bits);
    /* key algorithm */
    char *alg = NULL;
    (void) rnp_key_get_alg(key, &alg);
    fprintf(fp, "%s", cli_rnp_normalize_key_alg(alg));
#if defined(ENABLE_PQC)
    // in case of a SPHINCS+ key, also print the parameter set
    char *       param;
    rnp_result_t res = rnp_key_sphincsplus_get_param(key, &param);
    if (res == RNP_SUCCESS) {
        fprintf(fp, "-%s", param);
        rnp_buffer_destroy(param);
    }
#endif
    fprintf(fp, " ");
    /* key id */
    char *keyid = NULL;
    (void) rnp_key_get_keyid(key, &keyid);
    fprintf(fp, "%s", rnp::lowercase(keyid));
    /* key creation time */
    uint32_t create = 0;
    (void) rnp_key_get_creation(key, &create);
    fprintf(fp, " %s", ptimestr(buf, sizeof(buf), create));
    /* key usage */
    bool valid = false;
    bool expired = false;
    bool revoked = false;
    (void) rnp_key_is_valid(key, &valid);
    (void) rnp_key_is_expired(key, &expired);
    (void) rnp_key_is_revoked(key, &revoked);
    if (valid || expired || revoked) {
        fprintf(fp, " [%s]", cli_key_usage_str(key, buf));
    } else {
        fprintf(fp, " [INVALID]");
    }
    /* key expiration */
    uint32_t expiry = 0;
    (void) rnp_key_get_expiration(key, &expiry);
    if (expiry) {
        ptimestr(buf, sizeof(buf), create + expiry);
        fprintf(fp, " [%s %s]", expired ? "EXPIRED" : "EXPIRES", buf);
    }
    /* key is revoked */
    if (revoked) {
        fprintf(fp, " [REVOKED]");
    }
    /* fingerprint */
    char *keyfp = NULL;
    (void) rnp_key_get_fprint(key, &keyfp);
    fprintf(fp, "\n      %s\n", rnp::lowercase(keyfp));
    /* direct-key or binding signatures */
    if (psigs) {
        size_t sigs = 0;
        (void) rnp_key_get_signature_count(key, &sigs);
        for (size_t i = 0; i < sigs; i++) {
            rnp_signature_handle_t sig = NULL;
            (void) rnp_key_get_signature_at(key, i, &sig);
            if (!sig) {
                continue;
            }
            cli_rnp_print_sig_info(fp, ffi, sig);
            rnp_signature_handle_destroy(sig);
        }
    }
    /* user ids */
    size_t uids = 0;
    (void) rnp_key_get_uid_count(key, &uids);
    for (size_t i = 0; i < uids; i++) {
        rnp_uid_handle_t uid = NULL;

        if (rnp_key_get_uid_handle_at(key, i, &uid)) {
            continue;
        }
        bool  revoked = false;
        bool  valid = false;
        char *uid_str = NULL;
        (void) rnp_uid_is_revoked(uid, &revoked);
        (void) rnp_uid_is_valid(uid, &valid);
        (void) rnp_key_get_uid_at(key, i, &uid_str);

        /* userid itself with revocation status */
        fprintf(fp, "uid           %s", cli_rnp_escape_string(uid_str).c_str());
        fprintf(fp, "%s\n", revoked ? " [REVOKED]" : valid ? "" : " [INVALID]");
        rnp_buffer_destroy(uid_str);

        /* print signatures only if requested */
        if (!psigs) {
            (void) rnp_uid_handle_destroy(uid);
            continue;
        }

        size_t sigs = 0;
        (void) rnp_uid_get_signature_count(uid, &sigs);
        for (size_t j = 0; j < sigs; j++) {
            rnp_signature_handle_t sig = NULL;
            (void) rnp_uid_get_signature_at(uid, j, &sig);
            if (!sig) {
                continue;
            }
            cli_rnp_print_sig_info(fp, ffi, sig);
            rnp_signature_handle_destroy(sig);
        }
        (void) rnp_uid_handle_destroy(uid);
    }

    rnp_buffer_destroy(alg);
    rnp_buffer_destroy(keyid);
    rnp_buffer_destroy(keyfp);
}

bool
cli_rnp_save_keyrings(cli_rnp_t *rnp)
{
    rnp_output_t       output = NULL;
    rnp_result_t       pub_ret = 0;
    rnp_result_t       sec_ret = 0;
    const std::string &ppath = rnp->pubpath();
    const std::string &spath = rnp->secpath();

    // check whether we have G10 secret keyring - then need to create directory
    if (rnp->secformat() == "G10") {
        struct stat path_stat;
        if (rnp_stat(spath.c_str(), &path_stat) != -1) {
            if (!S_ISDIR(path_stat.st_mode)) {
                ERR_MSG("G10 keystore should be a directory: %s", spath.c_str());
                return false;
            }
        } else {
            if (errno != ENOENT) {
                ERR_MSG("stat(%s): %s", spath.c_str(), strerror(errno));
                return false;
            }
            if (RNP_MKDIR(spath.c_str(), S_IRWXU) != 0) {
                ERR_MSG("mkdir(%s, S_IRWXU): %s", spath.c_str(), strerror(errno));
                return false;
            }
        }
    }

    // public keyring
    if (!(pub_ret = rnp_output_to_path(&output, ppath.c_str()))) {
        pub_ret =
          rnp_save_keys(rnp->ffi, rnp->pubformat().c_str(), output, RNP_LOAD_SAVE_PUBLIC_KEYS);
        rnp_output_destroy(output);
    }
    if (pub_ret) {
        ERR_MSG("failed to write pubring to path '%s'", ppath.c_str());
    }

    // secret keyring
    if (!(sec_ret = rnp_output_to_path(&output, spath.c_str()))) {
        sec_ret =
          rnp_save_keys(rnp->ffi, rnp->secformat().c_str(), output, RNP_LOAD_SAVE_SECRET_KEYS);
        rnp_output_destroy(output);
    }
    if (sec_ret) {
        ERR_MSG("failed to write secring to path '%s'", spath.c_str());
    }

    return !pub_ret && !sec_ret;
}

bool
cli_rnp_generate_key(cli_rnp_t *rnp, const char *username)
{
    /* set key generation parameters to rnp_cfg_t */
    rnp_cfg &cfg = rnp->cfg();
    if (!cli_rnp_set_generate_params(cfg)) {
        ERR_MSG("Key generation setup failed.");
        return false;
    }
    /* generate the primary key */
    rnp_op_generate_t genkey = NULL;
    rnp_key_handle_t  primary = NULL;
    rnp_key_handle_t  subkey = NULL;
#if defined(ENABLE_PQC)
    rnp_key_handle_t subkey2 = NULL;
#endif
    bool res = false;

    if (rnp_op_generate_create(&genkey, rnp->ffi, cfg.get_cstr(CFG_KG_PRIMARY_ALG))) {
        ERR_MSG("Failed to initialize key generation.");
        return false;
    }
    if (username && rnp_op_generate_set_userid(genkey, username)) {
        ERR_MSG("Failed to set userid.");
        goto done;
    }
    if (cfg.has(CFG_KG_PRIMARY_BITS) &&
        rnp_op_generate_set_bits(genkey, cfg.get_int(CFG_KG_PRIMARY_BITS))) {
        ERR_MSG("Failed to set key bits.");
        goto done;
    }
    if (cfg.has(CFG_KG_PRIMARY_CURVE) &&
        rnp_op_generate_set_curve(genkey, cfg.get_cstr(CFG_KG_PRIMARY_CURVE))) {
        ERR_MSG("Failed to set key curve.");
        goto done;
    }
    if (cfg.has(CFG_KG_PRIMARY_EXPIRATION)) {
        uint32_t expiration = 0;
        if (!cfg.get_expiration(CFG_KG_PRIMARY_EXPIRATION, expiration) ||
            rnp_op_generate_set_expiration(genkey, expiration)) {
            ERR_MSG("Failed to set primary key expiration.");
            goto done;
        }
    }
    // TODO : set DSA qbits
    if (rnp_op_generate_set_hash(genkey, cfg.get_cstr(CFG_KG_HASH))) {
        ERR_MSG("Failed to set hash algorithm.");
        goto done;
    }

#if defined(ENABLE_CRYPTO_REFRESH)
    if (cfg.get_bool(CFG_KG_V6_KEY)) {
        rnp_op_generate_set_v6_key(genkey);
    }
#endif
#if defined(ENABLE_PQC)
    if (cfg.has(CFG_KG_PRIMARY_SPHINCSPLUS_PARAM) &&
        rnp_op_generate_set_sphincsplus_param(
          genkey, cfg.get_cstr(CFG_KG_PRIMARY_SPHINCSPLUS_PARAM))) {
        ERR_MSG("Failed to set sphincsplus parameter.");
        goto done;
    }
#endif

    fprintf(rnp->userio_out, "Generating a new key...\n");
    if (rnp_op_generate_execute(genkey) || rnp_op_generate_get_key(genkey, &primary)) {
        ERR_MSG("Primary key generation failed.");
        goto done;
    }

    if (!cfg.has(CFG_KG_SUBKEY_ALG)) {
        res = true;
        goto done;
    }

    rnp_op_generate_destroy(genkey);
    genkey = NULL;
    if (rnp_op_generate_subkey_create(
          &genkey, rnp->ffi, primary, cfg.get_cstr(CFG_KG_SUBKEY_ALG))) {
        ERR_MSG("Failed to initialize subkey generation.");
        goto done;
    }
    if (cfg.has(CFG_KG_SUBKEY_BITS) &&
        rnp_op_generate_set_bits(genkey, cfg.get_int(CFG_KG_SUBKEY_BITS))) {
        ERR_MSG("Failed to set subkey bits.");
        goto done;
    }
    if (cfg.has(CFG_KG_SUBKEY_CURVE) &&
        rnp_op_generate_set_curve(genkey, cfg.get_cstr(CFG_KG_SUBKEY_CURVE))) {
        ERR_MSG("Failed to set subkey curve.");
        goto done;
    }
    if (cfg.has(CFG_KG_SUBKEY_EXPIRATION)) {
        uint32_t expiration = 0;
        if (!cfg.get_expiration(CFG_KG_SUBKEY_EXPIRATION, expiration) ||
            rnp_op_generate_set_expiration(genkey, expiration)) {
            ERR_MSG("Failed to set subkey expiration.");
            goto done;
        }
    }
    // TODO : set DSA qbits
    if (rnp_op_generate_set_hash(genkey, cfg.get_cstr(CFG_KG_HASH))) {
        ERR_MSG("Failed to set hash algorithm.");
        goto done;
    }
#if defined(ENABLE_CRYPTO_REFRESH)
    if (cfg.get_bool(CFG_KG_V6_KEY)) {
        rnp_op_generate_set_v6_key(genkey);
    }
#endif
#if defined(ENABLE_PQC)
    if (cfg.has(CFG_KG_SUBKEY_SPHINCSPLUS_PARAM) &&
        rnp_op_generate_set_sphincsplus_param(genkey,
                                              cfg.get_cstr(CFG_KG_SUBKEY_SPHINCSPLUS_PARAM))) {
        ERR_MSG("Failed to set sphincsplus parameter.");
        goto done;
    }
#endif
    if (rnp_op_generate_execute(genkey) || rnp_op_generate_get_key(genkey, &subkey)) {
        ERR_MSG("Subkey generation failed.");
        goto done;
    }

#if defined(ENABLE_PQC)
    if (cfg.has(CFG_KG_SUBKEY_2_ALG)) {
        rnp_op_generate_destroy(genkey);
        genkey = NULL;
        if (rnp_op_generate_subkey_create(
              &genkey, rnp->ffi, primary, cfg.get_cstr(CFG_KG_SUBKEY_2_ALG))) {
            ERR_MSG("Failed to initialize subkey 2 generation.");
            goto done;
        }
        if (cfg.has(CFG_KG_SUBKEY_2_BITS) &&
            rnp_op_generate_set_bits(genkey, cfg.get_int(CFG_KG_SUBKEY_2_BITS))) {
            ERR_MSG("Failed to set subkey 2 bits.");
            goto done;
        }
        if (cfg.has(CFG_KG_SUBKEY_2_CURVE) &&
            rnp_op_generate_set_curve(genkey, cfg.get_cstr(CFG_KG_SUBKEY_2_CURVE))) {
            ERR_MSG("Failed to set subkey 2 curve.");
            goto done;
        }
        if (cfg.has(CFG_KG_SUBKEY_2_EXPIRATION)) {
            uint32_t expiration = 0;
            if (!cfg.get_expiration(CFG_KG_SUBKEY_2_EXPIRATION, expiration) ||
                rnp_op_generate_set_expiration(genkey, expiration)) {
                ERR_MSG("Failed to set subkey 2 expiration.");
                goto done;
            }
        }
        // TODO : set DSA qbits
        if (rnp_op_generate_set_hash(genkey, cfg.get_cstr(CFG_KG_HASH))) {
            ERR_MSG("Failed to set hash algorithm.");
            goto done;
        }
#if defined(ENABLE_CRYPTO_REFRESH)
        if (cfg.get_bool(CFG_KG_V6_KEY)) {
            rnp_op_generate_set_v6_key(genkey);
        }
#endif
        if (cfg.has(CFG_KG_SUBKEY_2_SPHINCSPLUS_PARAM) &&
            rnp_op_generate_set_sphincsplus_param(
              genkey, cfg.get_cstr(CFG_KG_SUBKEY_2_SPHINCSPLUS_PARAM))) {
            ERR_MSG("Failed to set sphincsplus parameter.");
            goto done;
        }
        if (rnp_op_generate_execute(genkey) || rnp_op_generate_get_key(genkey, &subkey2)) {
            ERR_MSG("Subkey generation failed.");
            goto done;
        }
    }
#endif

    // protect
#if defined(ENABLE_PQC)
    for (auto key : {primary, subkey, subkey2}) {
        if (!key) {
            continue;
        }
#else
    for (auto key : {primary, subkey}) {
#endif
        char *password = NULL;
        if (rnp_request_password(rnp->ffi, key, "protect", &password)) {
            ERR_MSG("Failed to obtain protection password.");
            goto done;
        }
        if (*password) {
            rnp_result_t ret = rnp_key_protect(key,
                                               password,
                                               cfg.get_cstr(CFG_KG_PROT_ALG),
                                               NULL,
                                               cfg.get_cstr(CFG_KG_PROT_HASH),
                                               cfg.get_int(CFG_KG_PROT_ITERATIONS));
            rnp_buffer_clear(password, strlen(password) + 1);
            rnp_buffer_destroy(password);
            if (ret) {
                ERR_MSG("Failed to protect key.");
                goto done;
            }
        } else {
            rnp_buffer_destroy(password);
        }
    }
    res = cli_rnp_save_keyrings(rnp);
done:
    if (res) {
        cli_rnp_print_key_info(stdout, rnp->ffi, primary, true, false);
        if (subkey) {
            cli_rnp_print_key_info(stdout, rnp->ffi, subkey, true, false);
        }
#if defined(ENABLE_PQC)
        if (subkey2) {
            cli_rnp_print_key_info(stdout, rnp->ffi, subkey2, true, false);
        }
#endif
    }
    rnp_op_generate_destroy(genkey);
    rnp_key_handle_destroy(primary);
    rnp_key_handle_destroy(subkey);
#if defined(ENABLE_PQC)
    rnp_key_handle_destroy(subkey2);
#endif
    return res;
}

static bool
key_matches_string(rnpffi::Key &key, const std::string &str)
{
    if (str.empty()) {
        return true;
    }
    if (rnp::is_hex(str) && (str.length() >= RNP_KEYID_SIZE)) {
        std::string hexstr = rnp::strip_hex(str);
        size_t      len = hexstr.length();

        /* check whether it's key id */
        if ((len == RNP_KEYID_SIZE * 2) || (len == RNP_KEYID_SIZE)) {
            auto keyid = key.keyid();
            if (keyid.size() < len) {
                return false;
            }
            if (!strncasecmp(hexstr.c_str(), keyid.c_str() + keyid.size() - len, len)) {
                return true;
            }
        }

        /* check fingerprint */
        auto keyfp = key.fprint();
        if ((len == keyfp.size()) && !strncasecmp(hexstr.c_str(), keyfp.c_str(), len)) {
            return true;
        }

        /* check grip */
        auto grip = key.grip();
        if (len == grip.size()) {
            if (!strncasecmp(hexstr.c_str(), grip.c_str(), len)) {
                return true;
            }
        }
        /* let then search for hex userid */
    }

    /* no need to check for userid over the subkey */
    if (key.is_sub()) {
        return false;
    }
    auto uid_count = key.uid_count();
    if (!uid_count) {
        return false;
    }

#ifndef RNP_USE_STD_REGEX
    regex_t r = {};
    /* match on full name or email address as a NOSUB, ICASE regexp */
    if (regcomp(&r, cli_rnp_unescape_for_regcomp(str).c_str(), REG_EXTENDED | REG_ICASE) !=
        0) {
        return false;
    }
#else
    std::regex re;
    try {
        re.assign(str, std::regex_constants::ECMAScript | std::regex_constants::icase);
    } catch (const std::exception &e) {
        ERR_MSG("Invalid regular expression : %s, error %s.", str.c_str(), e.what());
        return false;
    }
#endif

    bool matches = false;
    for (size_t idx = 0; idx < uid_count; idx++) {
        auto uid = key.uid_at(idx);
#ifndef RNP_USE_STD_REGEX
        if (regexec(&r, uid.c_str(), 0, NULL, 0) == 0) {
            matches = true;
            break;
        }
#else
        if (std::regex_search(uid, re)) {
            matches = true;
            break;
        }
#endif
    }
#ifndef RNP_USE_STD_REGEX
    regfree(&r);
#endif
    return matches;
}

static bool
key_matches_flags(rnpffi::Key &key, int flags)
{
    /* check whether secret key search is requested */
    if ((flags & CLI_SEARCH_SECRET) && !key.secret()) {
        return false;
    }
    /* check whether no subkeys allowed */
    if (!key.is_sub()) {
        return true;
    }
    if (!(flags & CLI_SEARCH_SUBKEYS)) {
        return false;
    }
    /* check whether subkeys should be put after primary (if it is available) */
    if ((flags & CLI_SEARCH_SUBKEYS_AFTER) != CLI_SEARCH_SUBKEYS_AFTER) {
        return true;
    }

    return key.primary_grip().empty();
}

void
clear_key_handles(std::vector<rnp_key_handle_t> &keys)
{
    for (auto handle : keys) {
        rnp_key_handle_destroy(handle);
    }
    keys.clear();
}

static bool
add_key_to_array(rnp_ffi_t                      ffi,
                 std::vector<rnp_key_handle_t> &keys,
                 rnp_key_handle_t               key,
                 int                            flags)
{
    bool subkey = false;
    bool subkeys = (flags & CLI_SEARCH_SUBKEYS_AFTER) == CLI_SEARCH_SUBKEYS_AFTER;
    if (rnp_key_is_sub(key, &subkey)) {
        return false;
    }

    try {
        keys.push_back(key);
    } catch (const std::exception &e) {
        ERR_MSG("%s", e.what());
        return false;
    }
    if (!subkeys || subkey) {
        return true;
    }

    std::vector<rnp_key_handle_t> subs;
    size_t                        sub_count = 0;
    if (rnp_key_get_subkey_count(key, &sub_count)) {
        goto error;
    }

    try {
        for (size_t i = 0; i < sub_count; i++) {
            rnp_key_handle_t sub_handle = NULL;
            if (rnp_key_get_subkey_at(key, i, &sub_handle)) {
                goto error;
            }
            subs.push_back(sub_handle);
        }
        std::move(subs.begin(), subs.end(), std::back_inserter(keys));
    } catch (const std::exception &e) {
        ERR_MSG("%s", e.what());
        goto error;
    }
    return true;
error:
    keys.pop_back();
    clear_key_handles(subs);
    return false;
}

bool
cli_rnp_t::keys_matching(std::vector<rnp_key_handle_t> &keys,
                         const std::string &            str,
                         int                            flags)
{
    rnpffi::FFI ffiobj(ffi, false);

    /* iterate through the keys */
    auto it = ffiobj.iterator_create("fingerprint");
    if (!it) {
        return false;
    }

    std::string fp;
    while (it->next(fp)) {
        auto key = ffiobj.locate_key("fingerprint", fp);
        if (!key) {
            continue;
        }
        if (!key_matches_flags(*key, flags) || !key_matches_string(*key, str)) {
            continue;
        }
        if (!add_key_to_array(ffi, keys, key->handle(), flags)) {
            return false;
        }
        key->release();
        if (flags & CLI_SEARCH_FIRST_ONLY) {
            return true;
        }
    }
    return !keys.empty();
}

bool
cli_rnp_t::keys_matching(std::vector<rnp_key_handle_t> & keys,
                         const std::vector<std::string> &strs,
                         int                             flags)
{
    clear_key_handles(keys);

    for (const std::string &str : strs) {
        if (!keys_matching(keys, str, flags & ~CLI_SEARCH_DEFAULT)) {
            ERR_MSG("Cannot find key matching \"%s\"", str.c_str());
            clear_key_handles(keys);
            return false;
        }
    }

    /* search for default key */
    if (keys.empty() && (flags & CLI_SEARCH_DEFAULT)) {
        if (defkey().empty()) {
            ERR_MSG("No userid or default key for operation");
            return false;
        }
        if (!keys_matching(keys, defkey(), flags & ~CLI_SEARCH_DEFAULT) || keys.empty()) {
            ERR_MSG("Default key not found");
        }
    }
    return !keys.empty();
}

std::unique_ptr<rnpffi::Key>
cli_rnp_t::key_matching(const std::string &str, int flags, size_t *count)
{
    std::vector<rnp_key_handle_t> keys;

    (void) keys_matching(keys, str, flags);
    if (count) {
        *count = keys.size();
    }
    if (keys.size() == 1) {
        auto res = new (std::nothrow) rnpffi::Key(keys[0]);
        if (!res) {
            rnp_key_handle_destroy(keys[0]);
        }
        return std::unique_ptr<rnpffi::Key>(res);
    }
    clear_key_handles(keys);
    return std::unique_ptr<rnpffi::Key>(nullptr);
}

static bool
rnp_cfg_set_ks_info(rnp_cfg &cfg)
{
    if (cfg.get_bool(CFG_KEYSTORE_DISABLED)) {
        cfg.set_str(CFG_KR_PUB_PATH, "");
        cfg.set_str(CFG_KR_SEC_PATH, "");
        cfg.set_str(CFG_KR_PUB_FORMAT, RNP_KEYSTORE_GPG);
        cfg.set_str(CFG_KR_SEC_FORMAT, RNP_KEYSTORE_GPG);
        return true;
    }

    /* getting path to keyrings. If it is specified by user in 'homedir' param then it is
     * considered as the final path */
    bool        defhomedir = false;
    std::string homedir = cfg.get_str(CFG_HOMEDIR);
    if (homedir.empty()) {
        homedir = rnp::path::HOME();
        defhomedir = true;
    }

    /* Check whether $HOME or homedir exists */
    struct stat st;
    if (rnp_stat(homedir.c_str(), &st) || rnp_access(homedir.c_str(), R_OK | W_OK)) {
        ERR_MSG("Home directory '%s' does not exist or is not writable!", homedir.c_str());
        return false;
    }

    /* creating home dir if needed */
    if (defhomedir) {
        char *rnphome = NULL;
        if (rnp_get_default_homedir(&rnphome)) {
            ERR_MSG("Failed to obtain default home directory.");
            return false;
        }
        homedir = rnphome;
        rnp_buffer_destroy(rnphome);
        if (!rnp::path::exists(homedir, true) && RNP_MKDIR(homedir.c_str(), 0700) == -1 &&
            errno != EEXIST) {
            ERR_MSG("Cannot mkdir '%s' errno = %d", homedir.c_str(), errno);
            return false;
        }
    }

    /* detecting key storage format */
    std::string ks_format = cfg.get_str(CFG_KEYSTOREFMT);
    if (ks_format.empty()) {
        char *pub_format = NULL;
        char *sec_format = NULL;
        char *pubpath = NULL;
        char *secpath = NULL;
        rnp_detect_homedir_info(homedir.c_str(), &pub_format, &pubpath, &sec_format, &secpath);
        bool detected = pub_format && sec_format && pubpath && secpath;
        if (detected) {
            cfg.set_str(CFG_KR_PUB_FORMAT, pub_format);
            cfg.set_str(CFG_KR_SEC_FORMAT, sec_format);
            cfg.set_str(CFG_KR_PUB_PATH, pubpath);
            cfg.set_str(CFG_KR_SEC_PATH, secpath);
        } else {
            /* default to GPG */
            ks_format = RNP_KEYSTORE_GPG;
        }
        rnp_buffer_destroy(pub_format);
        rnp_buffer_destroy(sec_format);
        rnp_buffer_destroy(pubpath);
        rnp_buffer_destroy(secpath);
        if (detected) {
            return true;
        }
    }

    std::string pub_format = RNP_KEYSTORE_GPG;
    std::string sec_format = RNP_KEYSTORE_GPG;
    std::string pubpath;
    std::string secpath;

    if (ks_format == RNP_KEYSTORE_GPG) {
        pubpath = rnp::path::append(homedir, PUBRING_GPG);
        secpath = rnp::path::append(homedir, SECRING_GPG);
    } else if (ks_format == RNP_KEYSTORE_GPG21) {
        pubpath = rnp::path::append(homedir, PUBRING_KBX);
        secpath = rnp::path::append(homedir, SECRING_G10);
        pub_format = RNP_KEYSTORE_KBX;
        sec_format = RNP_KEYSTORE_G10;
    } else if (ks_format == RNP_KEYSTORE_KBX) {
        pubpath = rnp::path::append(homedir, PUBRING_KBX);
        secpath = rnp::path::append(homedir, SECRING_KBX);
        pub_format = RNP_KEYSTORE_KBX;
        sec_format = RNP_KEYSTORE_KBX;
    } else if (ks_format == RNP_KEYSTORE_G10) {
        pubpath = rnp::path::append(homedir, PUBRING_G10);
        secpath = rnp::path::append(homedir, SECRING_G10);
        pub_format = RNP_KEYSTORE_G10;
        sec_format = RNP_KEYSTORE_G10;
    } else {
        ERR_MSG("Unsupported keystore format: \"%s\"", ks_format.c_str());
        return false;
    }

    /* Check whether homedir is empty */
    if (rnp::path::empty(homedir)) {
        ERR_MSG("Keyring directory '%s' is empty.\nUse \"rnpkeys\" command to generate a new "
                "key or import existing keys from the file or GnuPG keyrings.",
                homedir.c_str());
    }

    cfg.set_str(CFG_KR_PUB_PATH, pubpath);
    cfg.set_str(CFG_KR_SEC_PATH, secpath);
    cfg.set_str(CFG_KR_PUB_FORMAT, pub_format);
    cfg.set_str(CFG_KR_SEC_FORMAT, sec_format);
    return true;
}

static void
rnp_cfg_set_defkey(rnp_cfg &cfg)
{
    /* If a userid has been given, we'll use it. */
    std::string userid = cfg.get_count(CFG_USERID) ? cfg.get_str(CFG_USERID, 0) : "";
    if (!userid.empty()) {
        cfg.set_str(CFG_KR_DEF_KEY, userid);
    }
}

bool
cli_cfg_set_keystore_info(rnp_cfg &cfg)
{
    /* detecting keystore paths and format */
    if (!rnp_cfg_set_ks_info(cfg)) {
        return false;
    }

    /* default key/userid */
    rnp_cfg_set_defkey(cfg);
    return true;
}

static bool
is_stdinout_spec(const std::string &spec)
{
    return spec.empty() || (spec == "-");
}

rnp_input_t
cli_rnp_input_from_specifier(cli_rnp_t &rnp, const std::string &spec, bool *is_path)
{
    rnp_input_t  input = NULL;
    rnp_result_t res = RNP_ERROR_GENERIC;
    bool         path = false;
    if (is_stdinout_spec(spec)) {
        /* input from stdin */
        res = rnp_input_from_stdin(&input);
    } else if ((spec.size() > 4) && (spec.compare(0, 4, "env:") == 0)) {
        /* input from an environment variable */
        const char *envval = getenv(spec.c_str() + 4);
        if (!envval) {
            ERR_MSG("Failed to get value of the environment variable '%s'.", spec.c_str() + 4);
            return NULL;
        }
        res = rnp_input_from_memory(&input, (const uint8_t *) envval, strlen(envval), true);
    } else {
        /* input from path */
        res = rnp_input_from_path(&input, spec.c_str());
        path = true;
    }

    if (res) {
        return NULL;
    }
    if (is_path) {
        *is_path = path;
    }
    return input;
}

rnp_output_t
cli_rnp_output_to_specifier(cli_rnp_t &rnp, const std::string &spec, bool discard)
{
    rnp_output_t output = NULL;
    rnp_result_t res = RNP_ERROR_GENERIC;
    std::string  path = spec;
    if (discard) {
        res = rnp_output_to_null(&output);
    } else if (is_stdinout_spec(spec)) {
        res = rnp_output_to_stdout(&output);
    } else if (!rnp_get_output_filename(spec, path, rnp)) {
        if (spec.empty()) {
            ERR_MSG("Operation failed: no output filename specified");
        } else {
            ERR_MSG("Operation failed: file '%s' already exists.", spec.c_str());
        }
        res = RNP_ERROR_BAD_PARAMETERS;
    } else {
        res = rnp_output_to_file(&output, path.c_str(), RNP_OUTPUT_FILE_OVERWRITE);
    }
    return res ? NULL : output;
}

bool
cli_rnp_export_keys(cli_rnp_t *rnp, const char *filter)
{
    bool                          secret = rnp->cfg().get_bool(CFG_SECRET);
    int                           flags = secret ? CLI_SEARCH_SECRET : 0;
    std::vector<rnp_key_handle_t> keys;

    if (!rnp->keys_matching(keys, filter ? filter : std::string(), flags)) {
        ERR_MSG("Key(s) matching '%s' not found.", filter);
        return false;
    }

    rnp_output_t output = NULL;
    rnp_output_t armor = NULL;
    uint32_t     base_flags = secret ? RNP_KEY_EXPORT_SECRET : RNP_KEY_EXPORT_PUBLIC;
    bool         result = false;

    output = cli_rnp_output_to_specifier(*rnp, rnp->cfg().get_str(CFG_OUTFILE));
    if (!output) {
        goto done;
    }

    /* We need single armored stream for all of the keys */
    if (rnp_output_to_armor(output, &armor, secret ? "secret key" : "public key")) {
        goto done;
    }

    for (auto key : keys) {
        uint32_t flags = base_flags;
        bool     primary = false;

        if (rnp_key_is_primary(key, &primary)) {
            goto done;
        }
        if (primary) {
            flags = flags | RNP_KEY_EXPORT_SUBKEYS;
        }

        if (rnp_key_export(key, armor, flags)) {
            goto done;
        }
    }
    result = !rnp_output_finish(armor);
done:
    rnp_output_destroy(armor);
    rnp_output_destroy(output);
    clear_key_handles(keys);
    return result;
}

bool
cli_rnp_export_revocation(cli_rnp_t *rnp, const char *key)
{
    std::vector<rnp_key_handle_t> keys;
    if (!rnp->keys_matching(keys, key, 0)) {
        ERR_MSG("Key matching '%s' not found.", key);
        return false;
    }
    if (keys.size() > 1) {
        ERR_MSG("Ambiguous input: too many keys found for '%s'.", key);
        clear_key_handles(keys);
        return false;
    }
    rnp_output_t output = NULL;
    bool         result = false;

    output = cli_rnp_output_to_specifier(*rnp, rnp->cfg().get_str(CFG_OUTFILE));
    if (!output) {
        goto done;
    }

    result = !rnp_key_export_revocation(keys[0],
                                        output,
                                        RNP_KEY_EXPORT_ARMORED,
                                        rnp->cfg().get_cstr(CFG_HASH),
                                        rnp->cfg().get_cstr(CFG_REV_TYPE),
                                        rnp->cfg().get_cstr(CFG_REV_REASON));
done:
    rnp_output_destroy(output);
    clear_key_handles(keys);
    return result;
}

bool
cli_rnp_revoke_key(cli_rnp_t *rnp, const char *key)
{
    std::vector<rnp_key_handle_t> keys;
    if (!rnp->keys_matching(keys, key, CLI_SEARCH_SUBKEYS)) {
        ERR_MSG("Key matching '%s' not found.", key);
        return false;
    }
    bool         res = false;
    bool         revoked = false;
    rnp_result_t ret = 0;

    if (keys.size() > 1) {
        ERR_MSG("Ambiguous input: too many keys found for '%s'.", key);
        goto done;
    }
    if (rnp_key_is_revoked(keys[0], &revoked)) {
        ERR_MSG("Error getting key revocation status.");
        goto done;
    }
    if (revoked && !rnp->cfg().get_bool(CFG_FORCE)) {
        ERR_MSG("Error: key '%s' is revoked already. Use --force to generate another "
                "revocation signature.",
                key);
        goto done;
    }

    ret = rnp_key_revoke(keys[0],
                         0,
                         rnp->cfg().get_cstr(CFG_HASH),
                         rnp->cfg().get_cstr(CFG_REV_TYPE),
                         rnp->cfg().get_cstr(CFG_REV_REASON));
    if (ret) {
        ERR_MSG("Failed to revoke a key: error %d", (int) ret);
        goto done;
    }
    res = cli_rnp_save_keyrings(rnp);
    /* print info about the revoked key */
    if (res) {
        bool  subkey = false;
        char *grip = NULL;
        if (rnp_key_is_sub(keys[0], &subkey)) {
            ERR_MSG("Failed to get key info");
            goto done;
        }
        ret =
          subkey ? rnp_key_get_primary_grip(keys[0], &grip) : rnp_key_get_grip(keys[0], &grip);
        if (ret || !grip) {
            ERR_MSG("Failed to get primary key grip.");
            goto done;
        }
        clear_key_handles(keys);
        if (!rnp->keys_matching(keys, grip, CLI_SEARCH_SUBKEYS_AFTER)) {
            ERR_MSG("Failed to search for revoked key.");
            rnp_buffer_destroy(grip);
            goto done;
        }
        rnp_buffer_destroy(grip);
        for (auto handle : keys) {
            cli_rnp_print_key_info(rnp->userio_out, rnp->ffi, handle, false, false);
        }
    }
done:
    clear_key_handles(keys);
    return res;
}

bool
cli_rnp_remove_key(cli_rnp_t *rnp, const char *key)
{
    std::vector<rnp_key_handle_t> keys;
    if (!rnp->keys_matching(keys, key, CLI_SEARCH_SUBKEYS)) {
        ERR_MSG("Key matching '%s' not found.", key);
        return false;
    }
    bool         res = false;
    bool         secret = false;
    bool         primary = false;
    uint32_t     flags = RNP_KEY_REMOVE_PUBLIC;
    rnp_result_t ret = 0;

    if (keys.size() > 1) {
        ERR_MSG("Ambiguous input: too many keys found for '%s'.", key);
        goto done;
    }
    if (rnp_key_have_secret(keys[0], &secret)) {
        ERR_MSG("Error getting secret key presence.");
        goto done;
    }
    if (rnp_key_is_primary(keys[0], &primary)) {
        ERR_MSG("Key error.");
        goto done;
    }

    if (secret) {
        flags |= RNP_KEY_REMOVE_SECRET;
    }
    if (primary) {
        flags |= RNP_KEY_REMOVE_SUBKEYS;
    }

    if (secret && !rnp->cfg().get_bool(CFG_FORCE)) {
        if (!cli_rnp_get_confirmation(
              rnp,
              "Key '%s' has corresponding secret key. Do you really want to delete it?",
              key)) {
            goto done;
        }
    }

    ret = rnp_key_remove(keys[0], flags);

    if (ret) {
        ERR_MSG("Failed to remove the key: error %d", (int) ret);
        goto done;
    }
    res = cli_rnp_save_keyrings(rnp);
done:
    clear_key_handles(keys);
    return res;
}

bool
cli_rnp_add_key(cli_rnp_t *rnp)
{
    const std::string &path = rnp->cfg().get_str(CFG_KEYFILE);
    if (path.empty()) {
        return false;
    }

    rnp_input_t input = cli_rnp_input_from_specifier(*rnp, path, NULL);
    if (!input) {
        ERR_MSG("failed to open key from %s", path.c_str());
        return false;
    }

    bool res = !rnp_import_keys(
      rnp->ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS, NULL);
    rnp_input_destroy(input);

    // set default key if we didn't have one
    if (res && rnp->defkey().empty()) {
        rnp->set_defkey();
    }
    return res;
}

static bool
strip_extension(std::string &src)
{
    size_t dpos = src.find_last_of('.');
    if (dpos == std::string::npos) {
        return false;
    }
    src.resize(dpos);
    return true;
}

static bool
has_extension(const std::string &path, const std::string &ext)
{
    if (path.length() < ext.length()) {
        return false;
    }
    return path.compare(path.length() - ext.length(), ext.length(), ext) == 0;
}

static std::string
output_extension(const rnp_cfg &cfg, Operation op)
{
    switch (op) {
    case Operation::EncryptOrSign: {
        bool armor = cfg.get_bool(CFG_ARMOR);
        if (cfg.get_bool(CFG_DETACHED)) {
            return armor ? EXT_ASC : EXT_SIG;
        }
        if (cfg.get_bool(CFG_CLEARTEXT)) {
            return EXT_ASC;
        }
        return armor ? EXT_ASC : EXT_PGP;
    }
    case Operation::Enarmor:
        return EXT_ASC;
    default:
        return "";
    }
}

static bool
has_pgp_extension(const std::string &path)
{
    return has_extension(path, EXT_PGP) || has_extension(path, EXT_ASC) ||
           has_extension(path, EXT_GPG);
}

static std::string
output_strip_extension(Operation op, const std::string &in)
{
    std::string out = in;
    if ((op == Operation::Verify) && (has_pgp_extension(out))) {
        strip_extension(out);
        return out;
    }
    if ((op == Operation::Dearmor) && (has_extension(out, EXT_ASC))) {
        strip_extension(out);
        return out;
    }
    return "";
}

static std::string
extract_filename(const std::string &path)
{
    size_t lpos = path.find_last_of("/\\");
    if (lpos == std::string::npos) {
        return path;
    }
    return path.substr(lpos + 1);
}

bool
cli_rnp_t::init_io(Operation op, rnp_input_t *input, rnp_output_t *output)
{
    const std::string &in = cfg().get_str(CFG_INFILE);
    bool               is_pathin = true;
    if (input) {
        *input = cli_rnp_input_from_specifier(*this, in, &is_pathin);
        if (!*input) {
            return false;
        }
    }
    /* Update CFG_SETFNAME to insert into literal packet */
    if (!cfg().has(CFG_SETFNAME) && !is_pathin) {
        cfg().set_str(CFG_SETFNAME, "");
    }

    if (!output) {
        return true;
    }
    std::string out = cfg().get_str(CFG_OUTFILE);
    bool discard = (op == Operation::Verify) && out.empty() && cfg().get_bool(CFG_NO_OUTPUT);

    if (out.empty() && is_pathin && !discard) {
        /* Attempt to guess whether to add or strip extension for known cases */
        std::string ext = output_extension(cfg(), op);
        if (!ext.empty()) {
            out = in + ext;
        } else {
            out = output_strip_extension(op, in);
        }
    }

    *output = cli_rnp_output_to_specifier(*this, out, discard);
    if (!*output && input) {
        rnp_input_destroy(*input);
        *input = NULL;
    }
    return *output;
}

bool
cli_rnp_dump_file(cli_rnp_t *rnp)
{
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;
    uint32_t     flags = 0;
    uint32_t     jflags = 0;

    if (rnp->cfg().get_bool(CFG_GRIPS)) {
        flags |= RNP_DUMP_GRIP;
        jflags |= RNP_JSON_DUMP_GRIP;
    }
    if (rnp->cfg().get_bool(CFG_MPIS)) {
        flags |= RNP_DUMP_MPI;
        jflags |= RNP_JSON_DUMP_MPI;
    }
    if (rnp->cfg().get_bool(CFG_RAW)) {
        flags |= RNP_DUMP_RAW;
        jflags |= RNP_JSON_DUMP_RAW;
    }

    rnp_result_t ret = 0;
    if (!rnp->init_io(Operation::Dump, &input, &output)) {
        ERR_MSG("failed to open source or create output");
        ret = 1;
        goto done;
    }

    if (rnp->cfg().get_bool(CFG_JSON)) {
        char *json = NULL;
        ret = rnp_dump_packets_to_json(input, jflags, &json);
        if (!ret) {
            size_t len = strlen(json);
            size_t written = 0;
            ret = rnp_output_write(output, json, len, &written);
            if (written < len) {
                ret = RNP_ERROR_WRITE;
            }
            // add trailing empty line
            if (!ret) {
                ret = rnp_output_write(output, "\n", 1, &written);
            }
            if (written < 1) {
                ret = RNP_ERROR_WRITE;
            }
            rnp_buffer_destroy(json);
        }
    } else {
        ret = rnp_dump_packets_to_output(input, output, flags);
    }
    rnp_input_destroy(input);
    rnp_output_destroy(output);
done:
    return !ret;
}

bool
cli_rnp_armor_file(cli_rnp_t *rnp)
{
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;

    if (!rnp->init_io(Operation::Enarmor, &input, &output)) {
        ERR_MSG("failed to open source or create output");
        return false;
    }
    rnp_result_t ret = rnp_enarmor(input, output, rnp->cfg().get_cstr(CFG_ARMOR_DATA_TYPE));
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    return !ret;
}

bool
cli_rnp_dearmor_file(cli_rnp_t *rnp)
{
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;

    if (!rnp->init_io(Operation::Dearmor, &input, &output)) {
        ERR_MSG("failed to open source or create output");
        return false;
    }

    rnp_result_t ret = rnp_dearmor(input, output);
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    return !ret;
}

static bool
cli_rnp_sign(const rnp_cfg &cfg, cli_rnp_t *rnp, rnp_input_t input, rnp_output_t output)
{
    rnp_op_sign_t op = NULL;
    rnp_result_t  ret = RNP_ERROR_GENERIC;
    bool          cleartext = cfg.get_bool(CFG_CLEARTEXT);
    bool          detached = cfg.get_bool(CFG_DETACHED);

    if (cleartext) {
        ret = rnp_op_sign_cleartext_create(&op, rnp->ffi, input, output);
    } else if (detached) {
        ret = rnp_op_sign_detached_create(&op, rnp->ffi, input, output);
    } else {
        ret = rnp_op_sign_create(&op, rnp->ffi, input, output);
    }

    if (ret) {
        ERR_MSG("failed to initialize signing");
        return false;
    }

    /* setup sign operation via cfg */
    bool                          res = false;
    std::vector<std::string>      signers;
    std::vector<rnp_key_handle_t> signkeys;

    if (!cleartext) {
        rnp_op_sign_set_armor(op, cfg.get_bool(CFG_ARMOR));
    }

    if (!cleartext && !detached) {
        if (cfg.has(CFG_SETFNAME)) {
            if (rnp_op_sign_set_file_name(op, cfg.get_str(CFG_SETFNAME).c_str())) {
                goto done;
            }
        } else if (cfg.has(CFG_INFILE)) {
            const std::string &fname = cfg.get_str(CFG_INFILE);
            if (rnp_op_sign_set_file_name(op, extract_filename(fname).c_str())) {
                goto done;
            }
            rnp_op_sign_set_file_mtime(op, rnp_filemtime(fname.c_str()));
        }
        if (rnp_op_sign_set_compression(op, cfg.get_cstr(CFG_ZALG), cfg.get_int(CFG_ZLEVEL))) {
            goto done;
        }
    }

    if (rnp_op_sign_set_hash(op, cfg.get_hashalg().c_str())) {
        goto done;
    }
    rnp_op_sign_set_creation_time(op, cfg.get_sig_creation());
    {
        uint32_t expiration = 0;
        if (cfg.get_expiration(CFG_EXPIRATION, expiration)) {
            rnp_op_sign_set_expiration_time(op, expiration);
        }
    }

    /* signing keys */
    signers = cfg.get_list(CFG_SIGNERS);
    if (!rnp->keys_matching(signkeys,
                            signers,
                            CLI_SEARCH_SECRET | CLI_SEARCH_DEFAULT | CLI_SEARCH_SUBKEYS |
                              CLI_SEARCH_FIRST_ONLY)) {
        ERR_MSG("Failed to build signing keys list");
        goto done;
    }
    for (rnp_key_handle_t key : signkeys) {
        if (rnp_op_sign_add_signature(op, key, NULL)) {
            ERR_MSG("Failed to add signature");
            goto done;
        }
    }

    /* execute sign operation */
    res = !rnp_op_sign_execute(op);
done:
    clear_key_handles(signkeys);
    rnp_op_sign_destroy(op);
    return res;
}

static bool
cli_rnp_encrypt_and_sign(const rnp_cfg &cfg,
                         cli_rnp_t *    rnp,
                         rnp_input_t    input,
                         rnp_output_t   output)
{
    rnp_op_encrypt_t op = NULL;

    if (rnp_op_encrypt_create(&op, rnp->ffi, input, output)) {
        ERR_MSG("failed to initialize encryption");
        return false;
    }

    std::string                   fname;
    std::string                   aalg;
    std::vector<rnp_key_handle_t> enckeys;
    std::vector<rnp_key_handle_t> signkeys;
    bool                          res = false;
    rnp_result_t                  ret;

    rnp_op_encrypt_set_armor(op, cfg.get_bool(CFG_ARMOR));

    if (cfg.has(CFG_SETFNAME)) {
        if (rnp_op_encrypt_set_file_name(op, cfg.get_str(CFG_SETFNAME).c_str())) {
            goto done;
        }
    } else if (cfg.has(CFG_INFILE)) {
        const std::string &fname = cfg.get_str(CFG_INFILE);
        if (rnp_op_encrypt_set_file_name(op, extract_filename(fname).c_str())) {
            goto done;
        }
        rnp_op_encrypt_set_file_mtime(op, rnp_filemtime(fname.c_str()));
    }

    if (rnp_op_encrypt_set_compression(op, cfg.get_cstr(CFG_ZALG), cfg.get_int(CFG_ZLEVEL))) {
        goto done;
    }
    if (rnp_op_encrypt_set_cipher(op, cfg.get_cstr(CFG_CIPHER))) {
        goto done;
    }
    if (rnp_op_encrypt_set_hash(op, cfg.get_hashalg().c_str())) {
        goto done;
    }
    aalg = cfg.has(CFG_AEAD) ? cfg.get_str(CFG_AEAD) : "None";
    if (rnp_op_encrypt_set_aead(op, aalg.c_str())) {
        goto done;
    }
    if (cfg.has(CFG_AEAD_CHUNK) &&
        rnp_op_encrypt_set_aead_bits(op, cfg.get_int(CFG_AEAD_CHUNK))) {
        goto done;
    }
    if (cfg.has(CFG_NOWRAP) && rnp_op_encrypt_set_flags(op, RNP_ENCRYPT_NOWRAP)) {
        goto done;
    }

    /* adding passwords if password-based encryption is used */
    if (cfg.get_bool(CFG_ENCRYPT_SK)) {
        std::string halg = cfg.get_hashalg();
        std::string ealg = cfg.get_str(CFG_CIPHER);
        size_t      iterations = cfg.get_int(CFG_S2K_ITER);
        size_t      msec = cfg.get_int(CFG_S2K_MSEC);

        if (msec != DEFAULT_S2K_MSEC) {
            if (rnp_calculate_iterations(halg.c_str(), msec, &iterations)) {
                ERR_MSG("Failed to calculate S2K iterations");
                goto done;
            }
        }

        for (int i = 0; i < cfg.get_int(CFG_PASSWORDC, 1); i++) {
            if (rnp_op_encrypt_add_password(
                  op, NULL, halg.c_str(), iterations, ealg.c_str())) {
                ERR_MSG("Failed to add encrypting password");
                goto done;
            }
        }
    }

    /* adding encrypting keys if pk-encryption is used */
    if (cfg.get_bool(CFG_ENCRYPT_PK)) {
        std::vector<std::string> keynames = cfg.get_list(CFG_RECIPIENTS);
        if (!rnp->keys_matching(enckeys,
                                keynames,
                                CLI_SEARCH_DEFAULT | CLI_SEARCH_SUBKEYS |
                                  CLI_SEARCH_FIRST_ONLY)) {
            ERR_MSG("Failed to build recipients key list");
            goto done;
        }
        for (rnp_key_handle_t key : enckeys) {
            if (rnp_op_encrypt_add_recipient(op, key)) {
                ERR_MSG("Failed to add recipient");
                goto done;
            }
        }
    }

#if defined(ENABLE_CRYPTO_REFRESH)
    /* enable or disable v6 PKESK creation*/
    if (!cfg.get_bool(CFG_V3_PKESK_ONLY)) {
        rnp_op_encrypt_enable_pkesk_v6(op);
    }
#endif

    /* adding signatures if encrypt-and-sign is used */
    if (cfg.get_bool(CFG_SIGN_NEEDED)) {
        rnp_op_encrypt_set_creation_time(op, cfg.get_sig_creation());
        uint32_t expiration;
        if (cfg.get_expiration(CFG_EXPIRATION, expiration)) {
            rnp_op_encrypt_set_expiration_time(op, expiration);
        }

        /* signing keys */
        std::vector<std::string> keynames = cfg.get_list(CFG_SIGNERS);
        if (!rnp->keys_matching(signkeys,
                                keynames,
                                CLI_SEARCH_SECRET | CLI_SEARCH_DEFAULT | CLI_SEARCH_SUBKEYS |
                                  CLI_SEARCH_FIRST_ONLY)) {
            ERR_MSG("Failed to build signing keys list");
            goto done;
        }
        for (rnp_key_handle_t key : signkeys) {
            if (rnp_op_encrypt_add_signature(op, key, NULL)) {
                ERR_MSG("Failed to add signature");
                goto done;
            }
        }
    }

    /* execute encrypt or encrypt-and-sign operation */
    ret = rnp_op_encrypt_execute(op);
    res = (ret == RNP_SUCCESS);
    if (ret != RNP_SUCCESS) {
        ERR_MSG("Operation failed: %s", rnp_result_to_string(ret));
    }
done:
    clear_key_handles(signkeys);
    clear_key_handles(enckeys);
    rnp_op_encrypt_destroy(op);
    return res;
}

bool
cli_rnp_setup(cli_rnp_t *rnp)
{
    /* unset CFG_PASSWD and empty CFG_PASSWD are different cases */
    if (rnp->cfg().has(CFG_PASSWD)) {
        const std::string &passwd = rnp->cfg().get_str(CFG_PASSWD);
        if (rnp_ffi_set_pass_provider(
              rnp->ffi, ffi_pass_callback_string, (void *) passwd.c_str())) {
            return false;
        }
    }
    rnp->pswdtries = rnp->cfg().get_pswdtries();
    return true;
}

bool
cli_rnp_check_weak_hash(cli_rnp_t *rnp)
{
    if (rnp->cfg().has(CFG_WEAK_HASH)) {
        return true;
    }

    uint32_t security_level = 0;

    if (rnp_get_security_rule(rnp->ffi,
                              RNP_FEATURE_HASH_ALG,
                              rnp->cfg().get_hashalg().c_str(),
                              rnp->cfg().time(),
                              NULL,
                              NULL,
                              &security_level)) {
        ERR_MSG("Failed to get security rules for hash algorithm \'%s\'!",
                rnp->cfg().get_hashalg().c_str());
        return false;
    }

    if (security_level < RNP_SECURITY_DEFAULT) {
        ERR_MSG("Hash algorithm \'%s\' is cryptographically weak!",
                rnp->cfg().get_hashalg().c_str());
        return false;
    }
    /* TODO: check other weak algorithms and key sizes */
    return true;
}

bool
cli_rnp_check_old_ciphers(cli_rnp_t *rnp)
{
    if (rnp->cfg().has(CFG_ALLOW_OLD_CIPHERS)) {
        return true;
    }

    uint32_t security_level = 0;

    if (rnp_get_security_rule(rnp->ffi,
                              RNP_FEATURE_SYMM_ALG,
                              rnp->cfg().get_cipher().c_str(),
                              rnp->cfg().time(),
                              NULL,
                              NULL,
                              &security_level)) {
        ERR_MSG("Failed to get security rules for cipher algorithm \'%s\'!",
                rnp->cfg().get_cipher().c_str());
        return false;
    }

    if (security_level < RNP_SECURITY_DEFAULT) {
        ERR_MSG("Cipher algorithm \'%s\' is cryptographically weak!",
                rnp->cfg().get_cipher().c_str());
        return false;
    }
    /* TODO: check other weak algorithms and key sizes */
    return true;
}

bool
cli_rnp_protect_file(cli_rnp_t *rnp)
{
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;

    if (!rnp->init_io(Operation::EncryptOrSign, &input, &output)) {
        ERR_MSG("failed to open source or create output");
        return false;
    }

    bool res = false;
    bool sign = rnp->cfg().get_bool(CFG_SIGN_NEEDED);
    bool encrypt = rnp->cfg().get_bool(CFG_ENCRYPT_PK) || rnp->cfg().get_bool(CFG_ENCRYPT_SK);
    if (sign && !encrypt) {
        res = cli_rnp_sign(rnp->cfg(), rnp, input, output);
    } else if (encrypt) {
        res = cli_rnp_encrypt_and_sign(rnp->cfg(), rnp, input, output);
    } else {
        ERR_MSG("No operation specified");
    }

    rnp_input_destroy(input);
    rnp_output_destroy(output);
    return res;
}

/* helper function which prints something like 'using RSA (Sign-Only) key 0x0102030405060708 */
static void
cli_rnp_print_sig_key_info(FILE *resfp, rnp_signature_handle_t sig)
{
    char *keyid = NULL;
    char *alg = NULL;

    (void) rnp_signature_get_keyid(sig, &keyid);
    rnp::lowercase(keyid);
    (void) rnp_signature_get_alg(sig, &alg);

    fprintf(resfp,
            "using %s key %s\n",
            cli_rnp_normalize_key_alg(alg),
            keyid ? keyid : "0000000000000000");
    rnp_buffer_destroy(keyid);
    rnp_buffer_destroy(alg);
}

static void
cli_rnp_print_signatures(cli_rnp_t *rnp, const std::vector<rnp_op_verify_signature_t> &sigs)
{
    unsigned    invalidc = 0;
    unsigned    unknownc = 0;
    std::string title = "UNKNOWN signature";
    FILE *      resfp = rnp->resfp;

    for (auto sig : sigs) {
        rnp_result_t status = rnp_op_verify_signature_get_status(sig);
        switch (status) {
        case RNP_SUCCESS:
            title = "Good signature";
            break;
        case RNP_ERROR_SIGNATURE_EXPIRED:
            title = "EXPIRED signature";
            invalidc++;
            break;
        case RNP_ERROR_SIGNATURE_INVALID:
            title = "BAD signature";
            invalidc++;
            break;
        case RNP_ERROR_KEY_NOT_FOUND:
            title = "NO PUBLIC KEY for signature";
            unknownc++;
            break;
        case RNP_ERROR_SIGNATURE_UNKNOWN:
            title = "UNKNOWN signature";
            unknownc++;
            break;
        default:
            title = "UNKNOWN signature status";
            break;
        }

        if (status == RNP_ERROR_SIGNATURE_UNKNOWN) {
            fprintf(resfp, "%s\n", title.c_str());
            continue;
        }

        uint32_t create = 0;
        uint32_t expiry = 0;
        rnp_op_verify_signature_get_times(sig, &create, &expiry);

        time_t crtime = create;
        auto   str = rnp_ctime(crtime);
        fprintf(resfp,
                "%s made %s%s",
                title.c_str(),
                rnp_y2k38_warning(crtime) ? ">=" : "",
                str.c_str());
        if (expiry) {
            crtime = rnp_timeadd(crtime, expiry);
            str = rnp_ctime(crtime);
            fprintf(
              resfp, "Valid until %s%s\n", rnp_y2k38_warning(crtime) ? ">=" : "", str.c_str());
        }

        rnp_signature_handle_t handle = NULL;
        if (rnp_op_verify_signature_get_handle(sig, &handle)) {
            ERR_MSG("Failed to obtain signature handle.");
            continue;
        }

        cli_rnp_print_sig_key_info(resfp, handle);
        rnp_key_handle_t key = NULL;

        if ((status != RNP_ERROR_KEY_NOT_FOUND) && !rnp_signature_get_signer(handle, &key)) {
            cli_rnp_print_key_info(resfp, rnp->ffi, key, false, false);
            rnp_key_handle_destroy(key);
        }
        rnp_signature_handle_destroy(handle);
    }

    if (!sigs.size()) {
        ERR_MSG("No signature(s) found - is this a signed file?");
        return;
    }
    if (!invalidc && !unknownc) {
        ERR_MSG("Signature(s) verified successfully");
        return;
    }
    /* Show a proper error message if there are invalid/unknown signatures */
    auto si = invalidc > 1 ? "s" : "";
    auto su = unknownc > 1 ? "s" : "";
    auto fail = "Signature verification failure: ";
    if (invalidc && !unknownc) {
        ERR_MSG("%s%u invalid signature%s", fail, invalidc, si);
    } else if (!invalidc && unknownc) {
        ERR_MSG("%s%u unknown signature%s", fail, unknownc, su);
    } else {
        ERR_MSG("%s%u invalid signature%s, %u unknown signature%s",
                fail,
                invalidc,
                si,
                unknownc,
                su);
    }
}

static void
cli_rnp_inform_of_hidden_recipient(rnp_op_verify_t op)
{
    size_t recipients = 0;
    rnp_op_verify_get_recipient_count(op, &recipients);
    if (!recipients) {
        return;
    }
    for (size_t idx = 0; idx < recipients; idx++) {
        rnp_recipient_handle_t recipient = NULL;
        rnp_op_verify_get_recipient_at(op, idx, &recipient);
        char *keyid = NULL;
        rnp_recipient_get_keyid(recipient, &keyid);
        bool hidden = keyid && !strcmp(keyid, "0000000000000000");
        rnp_buffer_destroy(keyid);
        if (hidden) {
            ERR_MSG("Warning: message has hidden recipient, but it was ignored. Use "
                    "--allow-hidden to override this.");
            break;
        }
    }
}

bool
cli_rnp_process_file(cli_rnp_t *rnp)
{
    rnp_input_t input = NULL;
    if (!rnp->init_io(Operation::Verify, &input, NULL)) {
        ERR_MSG("failed to open source");
        return false;
    }

    char *contents = NULL;
    if (rnp_guess_contents(input, &contents)) {
        ERR_MSG("failed to check source contents");
        rnp_input_destroy(input);
        return false;
    }

    /* source data for detached signature verification */
    rnp_input_t                            source = NULL;
    rnp_output_t                           output = NULL;
    rnp_op_verify_t                        verify = NULL;
    rnp_result_t                           ret = RNP_ERROR_GENERIC;
    bool                                   res = false;
    std::vector<rnp_op_verify_signature_t> sigs;
    size_t                                 scount = 0;

    if (rnp::str_case_eq(contents, "signature")) {
        /* detached signature */
        std::string in = rnp->cfg().get_str(CFG_INFILE);
        std::string src = rnp->cfg().get_str(CFG_SOURCE);
        if (is_stdinout_spec(in) && is_stdinout_spec(src)) {
            ERR_MSG("Detached signature and signed source cannot be both stdin.");
            goto done;
        }
        if (src.empty() && !has_extension(in, EXT_SIG) && !has_extension(in, EXT_ASC)) {
            ERR_MSG("Unsupported detached signature extension. Use --source to override.");
            goto done;
        }
        if (src.empty()) {
            src = std::move(in);
            /* cannot fail as we checked for extension previously */
            strip_extension(src);
        }
        source = cli_rnp_input_from_specifier(*rnp, src, NULL);
        if (!source) {
            ERR_MSG("Failed to open source for detached signature verification.");
            goto done;
        }

        ret = rnp_op_verify_detached_create(&verify, rnp->ffi, source, input);
        if (!ret) {
            /* Currently CLI requires all signatures to be valid for success */
            ret = rnp_op_verify_set_flags(verify, RNP_VERIFY_REQUIRE_ALL_SIGS);
        }
    } else {
        if (!rnp->init_io(Operation::Verify, NULL, &output)) {
            ERR_MSG("Failed to create output stream.");
            goto done;
        }
        ret = rnp_op_verify_create(&verify, rnp->ffi, input, output);
        if (!ret) {
            uint32_t flags = 0;
            if (!rnp->cfg().get_bool(CFG_NO_OUTPUT)) {
                /* This would happen if user requested decryption instead of verification */
                flags = flags | RNP_VERIFY_IGNORE_SIGS_ON_DECRYPT;
            } else {
                /* Currently CLI requires all signatures to be valid for success */
                flags = flags | RNP_VERIFY_REQUIRE_ALL_SIGS;
            }
            if (rnp->cfg().get_bool(CFG_ALLOW_HIDDEN)) {
                /* Allow hidden recipient */
                flags = flags | RNP_VERIFY_ALLOW_HIDDEN_RECIPIENT;
            }
            ret = rnp_op_verify_set_flags(verify, flags);
        }
    }
    if (ret) {
        ERR_MSG("Failed to initialize verification/decryption operation.");
        goto done;
    }

    res = !rnp_op_verify_execute(verify);

    /* Check whether we had hidden recipient on verification/decryption failure */
    if (!res && !rnp->cfg().get_bool(CFG_ALLOW_HIDDEN)) {
        cli_rnp_inform_of_hidden_recipient(verify);
    }

    rnp_op_verify_get_signature_count(verify, &scount);
    if (!scount) {
        goto done;
    }

    for (size_t i = 0; i < scount; i++) {
        rnp_op_verify_signature_t sig = NULL;
        if (rnp_op_verify_get_signature_at(verify, i, &sig)) {
            ERR_MSG("Failed to obtain signature info.");
            res = false;
            goto done;
        }
        try {
            sigs.push_back(sig);
        } catch (const std::exception &e) {
            ERR_MSG("%s", e.what());
            res = false;
            goto done;
        }
    }
    cli_rnp_print_signatures(rnp, sigs);
done:
    rnp_buffer_destroy(contents);
    rnp_input_destroy(input);
    rnp_input_destroy(source);
    rnp_output_destroy(output);
    rnp_op_verify_destroy(verify);
    return res;
}

void
cli_rnp_print_praise(void)
{
    printf("%s\n%s\n", PACKAGE_STRING, PACKAGE_BUGREPORT);
    printf("Backend: %s\n", rnp_backend_string());
    printf("Backend version: %s\n", rnp_backend_version());
    printf("Supported algorithms:\n");
    cli_rnp_print_feature(stdout, RNP_FEATURE_PK_ALG, "Public key");
    cli_rnp_print_feature(stdout, RNP_FEATURE_SYMM_ALG, "Encryption");
    cli_rnp_print_feature(stdout, RNP_FEATURE_AEAD_ALG, "AEAD");
    cli_rnp_print_feature(stdout, RNP_FEATURE_PROT_MODE, "Key protection");
    cli_rnp_print_feature(stdout, RNP_FEATURE_HASH_ALG, "Hash");
    cli_rnp_print_feature(stdout, RNP_FEATURE_COMP_ALG, "Compression");
    cli_rnp_print_feature(stdout, RNP_FEATURE_CURVE, "Curves");
    printf("Please report security issues at (https://www.rnpgp.org/feedback) and\n"
           "general bugs at https://github.com/rnpgp/rnp/issues.\n");
}

void
cli_rnp_print_feature(FILE *fp, const char *type, const char *printed_type)
{
    char * result = NULL;
    size_t count;
    if (rnp_supported_features(type, &result) != RNP_SUCCESS) {
        ERR_MSG("Failed to list supported features: %s", type);
        return;
    }
    json_object *jso = json_tokener_parse(result);
    if (!jso) {
        ERR_MSG("Failed to parse JSON with features: %s", type);
        goto done;
    }
    fprintf(fp, "%s: ", printed_type);
    count = json_object_array_length(jso);
    for (size_t idx = 0; idx < count; idx++) {
        json_object *val = json_object_array_get_idx(jso, idx);
        fprintf(fp, " %s%s", json_object_get_string(val), idx < count - 1 ? "," : "");
    }
    fputs("\n", fp);
    fflush(fp);
    json_object_put(jso);
done:
    rnp_buffer_destroy(result);
}
