/*
 * Copyright (c) 2017-2019 [Ribose Inc](https://www.ribose.com).
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

#ifndef SUPPORT_H_
#define SUPPORT_H_

#include "config.h"
#include <string>
#include <vector>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#else
#include "uniwin.h"
#endif
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "rnp.h"
#include "rekey/rnp_key_store.h"
#include "../rnp/fficli.h"
#include "file-utils.h"
#include "crypto/mem.h"
#include "key.hpp"

#ifdef _WIN32
#define pipe(fds) _pipe(fds, 256, O_BINARY)
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
#endif
#ifndef HAVE_MKDTEMP
char *mkdtemp(char *templ);
#endif
#ifndef HAVE_REALPATH
#define realpath(N, R) _fullpath((R), (N), _MAX_PATH)
#endif

off_t file_size(const char *path);

/* Read file contents into the std::string */
std::string file_to_str(const std::string &path);

/* Read binary file contents into the vector */
std::vector<uint8_t> file_to_vec(const std::string &path);

/* Write string contents to the file */
void str_to_file(const std::string &path, const char *str);

/* Concatenate multiple strings into a full path.
 * A directory separator is added between components.
 * Must be called in between va_start and va_end.
 * Final argument of calling function must be NULL.
 */
void vpaths_concat(char *buffer, size_t buffer_size, const char *first, va_list ap);

/* Concatenate multiple strings into a full path.
 * Final argument must be NULL.
 */
char *paths_concat(char *buffer, size_t buffer_length, const char *first, ...);

/* Concatenate multiple strings into a full path and
 * check that the file exists.
 * Final argument must be NULL.
 */
int path_rnp_file_exists(const char *first, ...);

/* Concatenate multiple strings into a full path and
 * create the directory.
 * Final argument must be NULL.
 */
void path_mkdir(mode_t mode, const char *first, ...);

/* Recursively remove a directory.
 * The path must be a full path and must be located in /tmp, for safety.
 */
void delete_recursively(const char *path);

void copy_recursively(const char *src, const char *dst);

/* Creates and returns a temporary directory path.
 * Caller must free the string.
 */
char *make_temp_dir(void);

void clean_temp_dir(const char *path);

/* check whether bin value is equals hex string */
bool bin_eq_hex(const uint8_t *data, size_t len, const char *val);

bool hex2mpi(pgp::mpi *val, const char *hex);

/* check whether key id is equal to hex string */
bool cmp_keyid(const pgp::KeyID &id, const std::string &val);

/* check whether key fp is equal to hex string */
bool cmp_keyfp(const pgp::Fingerprint &fp, const std::string &val);

void test_ffi_init(rnp_ffi_t *ffi);

bool mpi_empty(const pgp::mpi &val);

bool rsa_sec_empty(const pgp::KeyMaterial &key);

bool rsa_sec_filled(const pgp::KeyMaterial &key);

bool write_pass_to_pipe(int fd, size_t count);
/* Setup readable pipe with default password inside */
bool setupPasswordfd(int *pipefd);

/* Common initialization of rnp structure : home path, keystore format and pointer to store
 * password fd */
bool setup_cli_rnp_common(cli_rnp_t * rnp,
                          const char *ks_format,
                          const char *homedir,
                          int *       pipefd);

/* Initialize key generation params with default values and specified hash algorithm */
void cli_set_default_rsa_key_desc(rnp_cfg &cfg, const char *hash);

// this is a password callback that will always fail
bool failing_password_callback(const pgp_password_ctx_t *ctx,
                               char *                    password,
                               size_t                    password_size,
                               void *                    userdata);

bool ffi_failing_password_provider(rnp_ffi_t        ffi,
                                   void *           app_ctx,
                                   rnp_key_handle_t key,
                                   const char *     pgp_context,
                                   char *           buf,
                                   size_t           buf_len);

// this is a password callback that should never be called
bool asserting_password_callback(const pgp_password_ctx_t *ctx,
                                 char *                    password,
                                 size_t                    password_size,
                                 void *                    userdata);

bool ffi_asserting_password_provider(rnp_ffi_t        ffi,
                                     void *           app_ctx,
                                     rnp_key_handle_t key,
                                     const char *     pgp_context,
                                     char *           buf,
                                     size_t           buf_len);

// this is a password callback that just copies the string in userdata to
// the password buffer
bool string_copy_password_callback(const pgp_password_ctx_t *ctx,
                                   char *                    password,
                                   size_t                    password_size,
                                   void *                    userdata);

bool ffi_string_password_provider(rnp_ffi_t        ffi,
                                  void *           app_ctx,
                                  rnp_key_handle_t key,
                                  const char *     pgp_context,
                                  char *           buf,
                                  size_t           buf_len);

void unused_getkeycb(rnp_ffi_t   ffi,
                     void *      app_ctx,
                     const char *identifier_type,
                     const char *identifier,
                     bool        secret);

bool unused_getpasscb(rnp_ffi_t        ffi,
                      void *           app_ctx,
                      rnp_key_handle_t key,
                      const char *     pgp_context,
                      char *           buf,
                      size_t           buf_len);

bool starts_with(const std::string &data, const std::string &match);
bool ends_with(const std::string &data, const std::string &match);

std::string fmt(const char *format, ...);
std::string strip_eol(const std::string &str);
std::string lowercase(const std::string &str);

bool check_json_field_str(json_object *      obj,
                          const std::string &field,
                          const std::string &value);
bool check_json_field_int(json_object *obj, const std::string &field, int value);
bool check_json_field_bool(json_object *obj, const std::string &field, bool value);
bool check_json_pkt_type(json_object *pkt, int tag);

rnp::Key *rnp_tests_get_key_by_id(rnp::KeyStore *    keyring,
                                  const std::string &keyid,
                                  rnp::Key *         after = NULL);
rnp::Key *rnp_tests_get_key_by_fpr(rnp::KeyStore *keyring, const std::string &keyid);
rnp::Key *rnp_tests_get_key_by_grip(rnp::KeyStore *keyring, const std::string &grip);
rnp::Key *rnp_tests_get_key_by_grip(rnp::KeyStore *keyring, const pgp::KeyGrip &grip);
rnp::Key *rnp_tests_key_search(rnp::KeyStore *keyring, const std::string &uid);

/* key load/reload  shortcuts */
void reload_pubring(rnp_ffi_t *ffi);
void reload_keyrings(rnp_ffi_t *ffi);
bool load_keys_gpg(rnp_ffi_t ffi, const std::string &pub, const std::string &sec = "");
bool load_keys_kbx_g10(rnp_ffi_t ffi, const std::string &pub, const std::string &sec = "");

/* key import shortcuts */
bool import_all_keys(rnp_ffi_t ffi, const std::string &path);
bool import_pub_keys(rnp_ffi_t ffi, const std::string &path);
bool import_sec_keys(rnp_ffi_t ffi, const std::string &path);
bool import_all_keys(rnp_ffi_t ffi, const uint8_t *data, size_t len, uint32_t flags = 0);
bool import_pub_keys(rnp_ffi_t ffi, const uint8_t *data, size_t len);
bool import_sec_keys(rnp_ffi_t ffi, const uint8_t *data, size_t len);
/* key export shortcut */
std::vector<uint8_t> export_key(rnp_key_handle_t key,
                                bool             armored = false,
                                bool             secret = false);
/* write transferable key(s) to stream */
bool write_transferable_key(pgp_transferable_key_t &key, pgp_dest_t &dst, bool armor = false);
bool write_transferable_keys(pgp_key_sequence_t &keys, pgp_dest_t *dst, bool armor = false);

/* Dump key to the stdout. Not used in real tests, but useful for artefact generation */
void dump_key_stdout(rnp_key_handle_t key, bool secret = false);

/* some shortcuts for less code */
bool     check_key_valid(rnp_key_handle_t key, bool validity);
bool     check_key_revoked(rnp_key_handle_t key, bool revoked);
bool     check_key_locked(rnp_key_handle_t key, bool locked);
uint32_t get_key_expiry(rnp_key_handle_t key);
size_t   get_key_uids(rnp_key_handle_t key);
bool     check_sub_valid(rnp_key_handle_t key, size_t idx, bool validity);
bool     check_uid_valid(rnp_key_handle_t key, size_t idx, bool valid);
bool     check_uid_primary(rnp_key_handle_t key, size_t idx, bool primary);
void     check_loaded_keys(const char *                    format,
                           bool                            armored,
                           uint8_t *                       buf,
                           size_t                          buf_len,
                           const char *                    id_type,
                           const std::vector<std::string> &expected_ids,
                           bool                            secret);
bool     check_key_grip(rnp_key_handle_t key, const std::string &expected);
bool     check_key_fp(rnp_key_handle_t key, const std::string &expected);
bool     check_key_revreason(rnp_key_handle_t key, const char *reason);
bool     check_has_key(rnp_ffi_t          ffi,
                       const std::string &id,
                       bool               secret = false,
                       bool               valid = true);
bool     check_sig_hash(rnp_signature_handle_t sig, const char *hash);
bool     check_sig_type(rnp_signature_handle_t sig, const char *type);
bool     check_sig_revreason(rnp_signature_handle_t sig,
                             const char *           revcode,
                             const char *           revreason);

rnp_key_handle_t get_key_by_fp(rnp_ffi_t ffi, const char *fp);
rnp_key_handle_t get_key_by_uid(rnp_ffi_t ffi, const char *uid);

/* create bogus key handle with NULL pub/sec keys */
rnp_key_handle_t bogus_key_handle(rnp_ffi_t ffi);

bool sm2_enabled();
bool aead_eax_enabled();
bool aead_ocb_enabled();
bool aead_ocb_aes_only();
bool twofish_enabled();
bool idea_enabled();
bool brainpool_enabled();
bool blowfish_enabled();
bool cast5_enabled();
bool ripemd160_enabled();

inline size_t
rnp_round_up(size_t n, size_t align_to)
{
    if (n % align_to || n == 0) {
        n += align_to - (n % align_to);
    }
    return n;
}

/* load g10/g23 gpg key and verify that it can be
   unprotected/protected
*/
bool test_load_gpg_check_key(rnp::KeyStore *pub, rnp::KeyStore *sec, const char *id);

#define MD5_FROM 1325376000
#define SHA1_DATA_FROM 1547856000
#define SHA1_KEY_FROM 1705629600
#define CAST5_3DES_IDEA_BLOWFISH_FROM 1727730000

#endif /* SUPPORT_H_ */
