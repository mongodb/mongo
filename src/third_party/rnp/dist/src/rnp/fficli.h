/*
 * Copyright (c) 2019-2023, [Ribose Inc](https://www.ribose.com).
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

#ifndef FFICLI_H_
#define FFICLI_H_

#include <stddef.h>
#include <stdbool.h>
#include <time.h>
#include "rnp/rnp.h"
#include "rnp/rnp_err.h"
#include "rnp/rnpcpp.hpp"
#include "config.h"
#include "rnpcfg.h"
#include "json.h"

enum class Operation { EncryptOrSign, Verify, Enarmor, Dearmor, Dump };

class cli_rnp_t {
  private:
    rnp_cfg cfg_{};
#ifdef _WIN32
    int    subst_argc{};
    char **subst_argv{};
#endif
    bool load_keyring(bool secret);
    bool is_cv25519_subkey(rnpffi::Key &key);
    bool get_protection(rnpffi::Key &key,
                        std::string &hash,
                        std::string &cipher,
                        size_t &     iterations);
    bool check_cv25519_bits(rnpffi::Key &key, rnpffi::String &prot_password, bool &tweaked);

  public:
    rnp_ffi_t   ffi{};
    FILE *      resfp{};      /* where to put result messages, defaults to stdout */
    FILE *      passfp{};     /* file pointer for password input */
    FILE *      userio_in{};  /* file pointer for user's inputs */
    FILE *      userio_out{}; /* file pointer for user's outputs */
    int         pswdtries{};  /* number of password tries, -1 for unlimited */
    size_t      reuse_password_for_subkey{}; // count of subkeys
    std::string reuse_primary_fprint;
    char *      reused_password{};
    bool        hidden_msg{}; /* true if hidden recipient message was displayed */

    static int ret_code(bool success);

    ~cli_rnp_t();
#ifdef _WIN32
    void substitute_args(int *argc, char ***argv);
#endif
    bool init(const rnp_cfg &cfg);
    void end();

    bool init_io(Operation op, rnp_input_t *input, rnp_output_t *output);

    bool load_keyrings(bool loadsecret = false);

    const std::string &
    defkey()
    {
        return cfg_.get_str(CFG_KR_DEF_KEY);
    }

    void set_defkey();

    const std::string &
    pubpath()
    {
        return cfg_.get_str(CFG_KR_PUB_PATH);
    }

    const std::string &
    secpath()
    {
        return cfg_.get_str(CFG_KR_SEC_PATH);
    }

    const std::string &
    pubformat()
    {
        return cfg_.get_str(CFG_KR_PUB_FORMAT);
    }

    const std::string &
    secformat()
    {
        return cfg_.get_str(CFG_KR_SEC_FORMAT);
    }

    rnp_cfg &
    cfg()
    {
        return cfg_;
    }

    bool fix_cv25519_subkey(const std::string &key, bool checkonly = false);

    bool add_new_subkey(const std::string &key);

    bool set_key_expire(const std::string &key);

    bool edit_key(const std::string &key);

    /**
     * @brief Find key(s) matching set of flags and search string.
     *
     * @param keys search results will be added here, leaving already existing items.
     * @param str search string: may be part of the userid, keyid, fingerprint or grip.
     * @param flags combination of the following flags:
     *              CLI_SEARCH_SECRET : require key to be secret,
     *              CLI_SEARCH_SUBKEYS : include subkeys to the results (see
     *                CLI_SEARCH_SUBKEYS_AFTER description).
     *              CLI_SEARCH_FIRST_ONLY : include only first key found
     *              CLI_SEARCH_SUBKEYS_AFTER : for each primary key add its subkeys after the
     * main key. This changes behaviour of subkey search, since those will be added only if
     * subkey is orphaned or primary key matches search.
     * @return true if operation succeeds and at least one key is found, or false otherwise.
     */

    bool keys_matching(std::vector<rnp_key_handle_t> &keys, const std::string &str, int flags);
    /**
     * @brief Find key(s) matching set of flags and search string(s).
     *
     * @param keys search results will be put here, overwriting vector's contents.
     * @param strs set of search strings, may be empty.
     * @param flags the same flags as for keys_matching(), except additional one:
     *              CLI_SEARCH_DEFAULT : if no key is found then default key from cli_rnp_t
     * will be searched.
     * @return true if operation succeeds and at least one key is found for each search string,
     * or false otherwise.
     */
    bool keys_matching(std::vector<rnp_key_handle_t> & keys,
                       const std::vector<std::string> &strs,
                       int                             flags);

    /**
     * @brief Find exactly one key, matching set of flags and search string.
     *
     * @param str search string, see keys_matching() for the details.
     * @param flags flags, see keys_matching() for the details.
     * @param count if non-nullptr, number of found keys with be stored here.
     * @return pointer to the key object if only single key was found, or nullptr otherwise.
     */
    std::unique_ptr<rnpffi::Key> key_matching(const std::string &str,
                                              int                flags,
                                              size_t *           count = nullptr);
};

typedef enum cli_search_flags_t {
    CLI_SEARCH_SECRET = 1 << 0,     /* search secret keys only */
    CLI_SEARCH_SUBKEYS = 1 << 1,    /* add subkeys as well */
    CLI_SEARCH_FIRST_ONLY = 1 << 2, /* return only first key matching */
    CLI_SEARCH_SUBKEYS_AFTER =
      (1 << 3) | CLI_SEARCH_SUBKEYS, /* put subkeys after the primary key */
    CLI_SEARCH_DEFAULT = 1 << 4      /* add default key if nothing found */
} cli_search_flags_t;

/**
 * @brief Set keystore parameters to the rnp_cfg_t. This includes keyring paths, types and
 *        default key.
 *
 * @param cfg pointer to the allocated rnp_cfg_t structure
 * @return true on success or false otherwise.
 * @return false
 */
bool cli_cfg_set_keystore_info(rnp_cfg &cfg);

/**
 * @brief Create input object from the specifier, which may represent:
 *        - path
 *        - stdin (if `-` or empty string is passed)
 *        - environment variable contents, if path looks like `env:VARIABLE_NAME`
 * @param rnp initialized CLI rnp object
 * @param spec specifier
 * @param is_path optional parameter. If specifier is path (not stdin, env variable), then true
 *                will be stored here, false otherwise. May be NULL if this information is not
 *                needed.
 * @return rnp_input_t object or NULL if operation failed.
 */
rnp_input_t cli_rnp_input_from_specifier(cli_rnp_t &        rnp,
                                         const std::string &spec,
                                         bool *             is_path);

/**
 * @brief Create output object from the specifier, which may represent:
 *        - path
 *        - stdout (if `-` or empty string is passed)
 *
 * @param rnp initialized CLI rnp object
 * @param spec specifier
 * @param discard just discard output
 * @return rnp_output_t  or NULL if operation failed.
 */
rnp_output_t cli_rnp_output_to_specifier(cli_rnp_t &        rnp,
                                         const std::string &spec,
                                         bool               discard = false);

bool cli_rnp_save_keyrings(cli_rnp_t *rnp);
void cli_rnp_print_key_info(
  FILE *fp, rnp_ffi_t ffi, rnp_key_handle_t key, bool psecret, bool psigs);
bool        cli_rnp_set_generate_params(rnp_cfg &cfg, bool subkey = false);
bool        cli_rnp_generate_key(cli_rnp_t *rnp, const char *username);
bool        cli_rnp_export_keys(cli_rnp_t *rnp, const char *filter);
bool        cli_rnp_export_revocation(cli_rnp_t *rnp, const char *key);
bool        cli_rnp_revoke_key(cli_rnp_t *rnp, const char *key);
bool        cli_rnp_remove_key(cli_rnp_t *rnp, const char *key);
bool        cli_rnp_add_key(cli_rnp_t *rnp);
bool        cli_rnp_dump_file(cli_rnp_t *rnp);
bool        cli_rnp_armor_file(cli_rnp_t *rnp);
bool        cli_rnp_dearmor_file(cli_rnp_t *rnp);
bool        cli_rnp_check_weak_hash(cli_rnp_t *rnp);
bool        cli_rnp_check_old_ciphers(cli_rnp_t *rnp);
bool        cli_rnp_setup(cli_rnp_t *rnp);
bool        cli_rnp_protect_file(cli_rnp_t *rnp);
bool        cli_rnp_process_file(cli_rnp_t *rnp);
std::string cli_rnp_escape_string(const std::string &src);
void        cli_rnp_print_praise(void);
void        cli_rnp_print_feature(FILE *fp, const char *type, const char *printed_type);
/**
 * @brief Convert algorithm name representation to one used by FFI.
 *        I.e. aes-128 to AES128, 3DES to TRIPLEDES, SHA-1 to SHA1 and so on.
 *
 * @param alg algorithm string
 * @return string with FFI algorithm's name. In case alias is not found the source string will
 * be returned.
 */
const std::string cli_rnp_alg_to_ffi(const std::string &alg);

/**
 * @brief Attempt to set hash algorithm using the value provided.
 *
 * @param cfg config
 * @param hash algorithm name.
 * @return true if algorithm is supported and set correctly, or false otherwise.
 */
bool cli_rnp_set_hash(rnp_cfg &cfg, const std::string &hash);

/**
 * @brief Attempt to set symmetric cipher algorithm using the value provided.
 *
 * @param cfg config
 * @param cipher algorithm name.
 * @return true if algorithm is supported and set correctly, or false otherwise.
 */
bool cli_rnp_set_cipher(rnp_cfg &cfg, const std::string &cipher);

void clear_key_handles(std::vector<rnp_key_handle_t> &keys);

const char *json_obj_get_str(json_object *obj, const char *key);

#ifdef _WIN32
bool rnp_win_substitute_cmdline_args(int *argc, char ***argv);
void rnp_win_clear_args(int argc, char **argv);
#endif

/* TODO: we should decide what to do with functions/constants/defines below */
#define RNP_FP_V4_SIZE 20
#if defined(ENABLE_CRYPTO_REFRESH)
#define RNP_PGP_VER_6 6
#define RNP_FP_V6_SIZE 32
#endif
#define RNP_KEYID_SIZE 8
#define RNP_GRIP_SIZE 20

#define ERR_MSG(...)                           \
    do {                                       \
        (void) fprintf((stderr), __VA_ARGS__); \
        (void) fprintf((stderr), "\n");        \
    } while (0)

#define EXT_ASC (".asc")
#define EXT_SIG (".sig")
#define EXT_PGP (".pgp")
#define EXT_GPG (".gpg")

#define SUBDIRECTORY_GNUPG ".gnupg"
#define SUBDIRECTORY_RNP ".rnp"
#define PUBRING_KBX "pubring.kbx"
#define SECRING_KBX "secring.kbx"
#define PUBRING_GPG "pubring.gpg"
#define SECRING_GPG "secring.gpg"
#define PUBRING_G10 "public-keys-v1.d"
#define SECRING_G10 "private-keys-v1.d"

#endif
