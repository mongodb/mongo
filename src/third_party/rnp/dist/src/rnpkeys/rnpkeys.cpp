/*
 * Copyright (c) 2017-2023, [Ribose Inc](https://www.ribose.com).
 * Copyright (c) 2009 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is originally derived from software contributed to
 * The NetBSD Foundation by Alistair Crooks (agc@netbsd.org), and
 * carried further by Ribose Inc (https://www.ribose.com).
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/* Command line program to perform rnp operations */

#ifdef _MSC_VER
#include "uniwin.h"
#else
#include <getopt.h>
#endif
#include <string.h>
#include <stdarg.h>
#include "rnpkeys.h"
#include "str-utils.h"
#include <set>

const char *usage =
  "Manipulate OpenPGP keys and keyrings.\n"
  "Usage: rnpkeys --command [options] [files]\n"
  "Commands:\n"
  "  -h, --help              This help message.\n"
  "  -V, --version           Print RNP version information.\n"
  "  -g, --generate-key      Generate a new keypair (default is RSA).\n"
  "    --userid              Specify key's userid.\n"
  "    --expert              Select key type, size, and additional parameters.\n"
  "    --numbits             Override default key size (2048).\n"
  "    --expiration          Set key and subkey expiration time.\n"
  "    --cipher              Set cipher used to encrypt a secret key.\n"
  "    --hash                Set hash which is used for key derivation.\n"
  "    --allow-weak-hash     Allow usage of a weak hash algorithm.\n"
  "    --allow-sha1-key-sigs Allow usage of a SHA-1 key signatures.\n"
  "  -l, --list-keys         List keys in the keyrings.\n"
  "    --secret              List secret keys instead of public ones.\n"
  "    --with-sigs           List signatures as well.\n"
  "  --import                Import keys or signatures.\n"
  "  --import-keys           Import keys.\n"
  "  --import-sigs           Import signatures.\n"
  "    --permissive          Skip erroring keys/sigs instead of failing.\n"
  "  --export-key            Export a key.\n"
  "    --secret              Export a secret key instead of a public.\n"
  "  --export-rev            Export a key's revocation.\n"
  "    --rev-type            Set revocation type.\n"
  "    --rev-reason          Human-readable reason for revocation.\n"
  "  --revoke-key            Revoke a key specified.\n"
  "  --remove-key            Remove a key specified.\n"
  "  --edit-key              Edit key properties.\n"
  "    --add-subkey          Add new subkey.\n"
  "    --check-cv25519-bits  Check whether Cv25519 subkey bits are correct.\n"
  "    --fix-cv25519-bits    Fix Cv25519 subkey bits.\n"
  "    --set-expire          Set key expiration time.\n"
  "\n"
  "Other options:\n"
  "  --homedir               Override home directory (default is ~/.rnp/).\n"
  "  --password              Password, which should be used during operation.\n"
  "  --pass-fd               Read password(s) from the file descriptor.\n"
  "  --force                 Force operation (like secret key removal).\n"
  "  --keyfile               Load key(s) only from the file specified.\n"
  "  --output [file, -]      Write data to the specified file or stdout.\n"
  "  --overwrite             Overwrite output file without a prompt.\n"
  "  --notty                 Do not write anything to the TTY.\n"
  "  --current-time          Override system's time.\n"
  "  --allow-old-ciphers     Allow to use 64-bit ciphers (CAST5, 3DES, IDEA, BLOWFISH).\n"
  "\n"
  "See man page for a detailed listing and explanation.\n"
  "\n";

struct option options[] = {
  /* key-management commands */
  {"list-keys", no_argument, NULL, CMD_LIST_KEYS},
  {"export", no_argument, NULL, CMD_EXPORT_KEY},
  {"export-key", optional_argument, NULL, CMD_EXPORT_KEY},
  {"import", no_argument, NULL, CMD_IMPORT},
  {"import-key", no_argument, NULL, CMD_IMPORT_KEYS},
  {"import-keys", no_argument, NULL, CMD_IMPORT_KEYS},
  {"import-sigs", no_argument, NULL, CMD_IMPORT_SIGS},
  {"gen", optional_argument, NULL, CMD_GENERATE_KEY},
  {"gen-key", optional_argument, NULL, CMD_GENERATE_KEY},
  {"generate", optional_argument, NULL, CMD_GENERATE_KEY},
  {"generate-key", optional_argument, NULL, CMD_GENERATE_KEY},
  {"export-rev", no_argument, NULL, CMD_EXPORT_REV},
  {"export-revocation", no_argument, NULL, CMD_EXPORT_REV},
  {"revoke-key", no_argument, NULL, CMD_REVOKE_KEY},
  {"remove-key", no_argument, NULL, CMD_REMOVE_KEY},
  {"edit-key", no_argument, NULL, CMD_EDIT_KEY},
  /* debugging commands */
  {"help", no_argument, NULL, CMD_HELP},
  {"version", no_argument, NULL, CMD_VERSION},
  {"debug", required_argument, NULL, OPT_DEBUG},
  /* options */
  {"coredumps", no_argument, NULL, OPT_COREDUMPS},
  {"keystore-format", required_argument, NULL, OPT_KEY_STORE_FORMAT},
  {"userid", required_argument, NULL, OPT_USERID},
  {"with-sigs", no_argument, NULL, OPT_WITH_SIGS},
  {"hash", required_argument, NULL, OPT_HASH_ALG},
  {"home", required_argument, NULL, OPT_HOMEDIR},
  {"homedir", required_argument, NULL, OPT_HOMEDIR},
  {"numbits", required_argument, NULL, OPT_NUMBITS},
  {"s2k-iterations", required_argument, NULL, OPT_S2K_ITER},
  {"s2k-msec", required_argument, NULL, OPT_S2K_MSEC},
  {"expiration", required_argument, NULL, OPT_EXPIRATION},
  {"pass-fd", required_argument, NULL, OPT_PASSWDFD},
  {"password", required_argument, NULL, OPT_PASSWD},
  {"results", required_argument, NULL, OPT_RESULTS},
  {"cipher", required_argument, NULL, OPT_CIPHER},
  {"expert", no_argument, NULL, OPT_EXPERT},
  {"output", required_argument, NULL, OPT_OUTPUT},
  {"overwrite", no_argument, NULL, OPT_OVERWRITE},
  {"force", no_argument, NULL, OPT_FORCE},
  {"secret", no_argument, NULL, OPT_SECRET},
  {"rev-type", required_argument, NULL, OPT_REV_TYPE},
  {"rev-reason", required_argument, NULL, OPT_REV_REASON},
  {"permissive", no_argument, NULL, OPT_PERMISSIVE},
  {"notty", no_argument, NULL, OPT_NOTTY},
  {"fix-cv25519-bits", no_argument, NULL, OPT_FIX_25519_BITS},
  {"check-cv25519-bits", no_argument, NULL, OPT_CHK_25519_BITS},
  {"add-subkey", no_argument, NULL, OPT_ADD_SUBKEY},
  {"set-expire", required_argument, NULL, OPT_SET_EXPIRE},
  {"current-time", required_argument, NULL, OPT_CURTIME},
  {"allow-old-ciphers", no_argument, NULL, OPT_ALLOW_OLD_CIPHERS},
  {"allow-weak-hash", no_argument, NULL, OPT_ALLOW_WEAK_HASH},
  {"allow-sha1-key-sigs", no_argument, NULL, OPT_ALLOW_SHA1},
  {"keyfile", required_argument, NULL, OPT_KEYFILE},
  {NULL, 0, NULL, 0},
};

/* list keys */
static bool
print_keys_info(cli_rnp_t *rnp, FILE *fp, const char *filter)
{
    bool psecret = rnp->cfg().get_bool(CFG_SECRET);
    bool psigs = rnp->cfg().get_bool(CFG_WITH_SIGS);
    int  flags = CLI_SEARCH_SUBKEYS_AFTER | (psecret ? CLI_SEARCH_SECRET : 0);
    std::vector<rnp_key_handle_t> keys;

    if (!rnp->keys_matching(keys, filter ? filter : "", flags)) {
        fprintf(fp, "Key(s) not found.\n");
        return false;
    }
    fprintf(fp, "%d key%s found\n", (int) keys.size(), (keys.size() == 1) ? "" : "s");
    for (auto key : keys) {
        cli_rnp_print_key_info(fp, rnp->ffi, key, psecret, psigs);
    }

    fprintf(fp, "\n");
    /* clean up */
    clear_key_handles(keys);
    return true;
}

static bool
import_keys(cli_rnp_t *rnp, rnp_input_t input, const std::string &inname)
{
    std::set<std::string> new_pub_keys;
    std::set<std::string> new_sec_keys;
    std::set<std::string> updated_keys;
    bool                  res = false;
    bool                  updated = false;
    size_t                unchanged_keys = 0;
    size_t                processed_keys = 0;

    uint32_t flags = RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS |
                     RNP_LOAD_SAVE_SINGLE | RNP_LOAD_SAVE_BASE64;

    bool permissive = rnp->cfg().get_bool(CFG_PERMISSIVE);
    if (permissive) {
        flags |= RNP_LOAD_SAVE_PERMISSIVE;
    }

    do {
        /* load keys one-by-one */
        char *       results = NULL;
        rnp_result_t ret = rnp_import_keys(rnp->ffi, input, flags, &results);
        if (ret == RNP_ERROR_EOF) {
            res = true;
            break;
        }
        if (ret && updated) {
            /* some keys were imported, but then error occurred */
            ERR_MSG("warning: not all data was processed.");
            res = true;
            break;
        }
        if (ret) {
            ERR_MSG("failed to import key(s) from %s, stopping.", inname.c_str());
            break;
        }

        // print information about imported key(s)
        json_object *jso = json_tokener_parse(results);
        rnp_buffer_destroy(results);
        if (!jso) {
            ERR_MSG("invalid key import resulting JSON");
            break;
        }
        json_object *keys = NULL;
        if (!json_object_object_get_ex(jso, "keys", &keys)) {
            ERR_MSG("invalid key import JSON contents");
            json_object_put(jso);
            break;
        }
        processed_keys += json_object_array_length(keys);
        for (size_t idx = 0; idx < (size_t) json_object_array_length(keys); idx++) {
            json_object *    keyinfo = json_object_array_get_idx(keys, idx);
            rnp_key_handle_t key = NULL;
            if (!keyinfo) {
                continue;
            }
            std::string pub_status = json_obj_get_str(keyinfo, "public");
            std::string sec_status = json_obj_get_str(keyinfo, "secret");
            const char *fphex = json_obj_get_str(keyinfo, "fingerprint");

            if (pub_status == "new") {
                new_pub_keys.insert(fphex);
                updated = true;
            }
            if (sec_status == "new") {
                new_sec_keys.insert(fphex);
                updated = true;
            }
            if (pub_status == "updated" || sec_status == "updated") {
                updated_keys.insert(fphex);
                updated = true;
            }
            if (pub_status == "unchanged" || sec_status == "unchanged") {
                if (!new_pub_keys.count(fphex) && !new_sec_keys.count(fphex) &&
                    !updated_keys.count(fphex)) {
                    unchanged_keys++;
                    continue;
                }
            }
            if (rnp_locate_key(rnp->ffi, "fingerprint", fphex, &key) || !key) {
                ERR_MSG("failed to locate key with fingerprint %s", fphex);
                continue;
            }
            cli_rnp_print_key_info(stdout, rnp->ffi, key, true, false);
            rnp_key_handle_destroy(key);
        }
        json_object_put(jso);
    } while (1);

    // print statistics
    ERR_MSG("Import finished: %zu key%s processed, %zu new public keys, %zu new secret keys, "
            "%zu updated, %zu unchanged.",
            processed_keys,
            (processed_keys != 1) ? "s" : "",
            new_pub_keys.size(),
            new_sec_keys.size(),
            updated_keys.size(),
            unchanged_keys);

    if (updated) {
        // set default key if we didn't have one
        if (rnp->defkey().empty()) {
            rnp->set_defkey();
        }

        // save public and secret keyrings
        if (!cli_rnp_save_keyrings(rnp)) {
            ERR_MSG("failed to save keyrings");
        }
    }
    return res;
}

static bool
import_sigs(cli_rnp_t *rnp, rnp_input_t input, const std::string &inname)
{
    bool         res = false;
    char *       results = NULL;
    json_object *jso = NULL;
    json_object *sigs = NULL;
    int          unknown_sigs = 0;
    int          new_sigs = 0;
    int          old_sigs = 0;

    if (rnp_import_signatures(rnp->ffi, input, 0, &results)) {
        ERR_MSG("Failed to import signatures from %s", inname.c_str());
        goto done;
    }
    // print information about imported signature(s)
    jso = json_tokener_parse(results);
    if (!jso || !json_object_object_get_ex(jso, "sigs", &sigs)) {
        ERR_MSG("Invalid signature import result");
        goto done;
    }

    for (size_t idx = 0; idx < (size_t) json_object_array_length(sigs); idx++) {
        json_object *siginfo = json_object_array_get_idx(sigs, idx);
        if (!siginfo) {
            continue;
        }
        const char *status = json_obj_get_str(siginfo, "public");
        std::string pub_status = status ? status : "unknown";
        status = json_obj_get_str(siginfo, "secret");
        std::string sec_status = status ? status : "unknown";

        if ((pub_status == "new") || (sec_status == "new")) {
            new_sigs++;
        } else if ((pub_status == "unchanged") || (sec_status == "unchanged")) {
            old_sigs++;
        } else {
            unknown_sigs++;
        }
    }

    // print status information
    ERR_MSG("Import finished: %d new signature%s, %d unchanged, %d unknown.",
            new_sigs,
            (new_sigs != 1) ? "s" : "",
            old_sigs,
            unknown_sigs);

    // save public and secret keyrings
    if ((new_sigs > 0) && !cli_rnp_save_keyrings(rnp)) {
        ERR_MSG("Failed to save keyrings");
        goto done;
    }
    res = true;
done:
    json_object_put(jso);
    rnp_buffer_destroy(results);
    return res;
}

static bool
import(cli_rnp_t *rnp, const std::string &spec, int cmd)
{
    if (spec.empty()) {
        ERR_MSG("Import path isn't specified");
        return false;
    }
    rnp_input_t input = cli_rnp_input_from_specifier(*rnp, spec, NULL);
    if (!input) {
        ERR_MSG("Failed to create input for %s", spec.c_str());
        return false;
    }
    if (cmd == CMD_IMPORT) {
        char *contents = NULL;
        if (rnp_guess_contents(input, &contents)) {
            ERR_MSG("Warning! Failed to guess content type to import. Assuming keys.");
        }
        cmd = (contents && !strcmp(contents, "signature")) ? CMD_IMPORT_SIGS : CMD_IMPORT_KEYS;
        rnp_buffer_destroy(contents);
    }

    bool res = false;
    switch (cmd) {
    case CMD_IMPORT_KEYS:
        res = import_keys(rnp, input, spec);
        break;
    case CMD_IMPORT_SIGS:
        res = import_sigs(rnp, input, spec);
        break;
    default:
        ERR_MSG("Unexpected command: %d", cmd);
    }
    rnp_input_destroy(input);
    return res;
}

/* print a usage message */
void
print_usage(const char *usagemsg)
{
    cli_rnp_print_praise();
    puts(usagemsg);
}

/* do a command once for a specified file 'f' */
bool
rnp_cmd(cli_rnp_t *rnp, optdefs_t cmd, const char *f)
{
    std::string fs;

    switch (cmd) {
    case CMD_LIST_KEYS:
        if (!f && rnp->cfg().get_count(CFG_USERID)) {
            fs = rnp->cfg().get_str(CFG_USERID, 0);
            f = fs.c_str();
        }
        return print_keys_info(rnp, stdout, f);
    case CMD_EXPORT_KEY: {
        if (!f && rnp->cfg().get_count(CFG_USERID)) {
            fs = rnp->cfg().get_str(CFG_USERID, 0);
            f = fs.c_str();
        }
        return cli_rnp_export_keys(rnp, f);
    }
    case CMD_IMPORT:
    case CMD_IMPORT_KEYS:
    case CMD_IMPORT_SIGS:
        return import(rnp, f ? f : "", cmd);
    case CMD_GENERATE_KEY: {
        if (!f) {
            size_t count = rnp->cfg().get_count(CFG_USERID);
            if (count == 1) {
                fs = rnp->cfg().get_str(CFG_USERID, 0);
                f = fs.c_str();
            } else if (count > 1) {
                ERR_MSG("Only single userid is supported for generated keys");
                return false;
            }
        }
        return cli_rnp_generate_key(rnp, f);
    }
    case CMD_EXPORT_REV: {
        if (!f) {
            ERR_MSG("You need to specify key to generate revocation for.");
            return false;
        }
        return cli_rnp_export_revocation(rnp, f);
    }
    case CMD_REVOKE_KEY: {
        if (!f) {
            ERR_MSG("You need to specify key or subkey to revoke.");
            return false;
        }
        return cli_rnp_revoke_key(rnp, f);
    }
    case CMD_REMOVE_KEY: {
        if (!f) {
            ERR_MSG("You need to specify key or subkey to remove.");
            return false;
        }
        return cli_rnp_remove_key(rnp, f);
    }
    case CMD_EDIT_KEY: {
        if (!f) {
            ERR_MSG("You need to specify a key or subkey to edit.");
            return false;
        }
        return rnp->edit_key(f);
    }
    case CMD_VERSION:
        cli_rnp_print_praise();
        return true;
    case CMD_HELP:
    default:
        print_usage(usage);
        return true;
    }
}

/* set the option */
bool
setoption(rnp_cfg &cfg, optdefs_t *cmd, int val, const char *arg)
{
    switch (val) {
    case OPT_COREDUMPS:
#ifdef _WIN32
        ERR_MSG("warning: --coredumps doesn't make sense on windows systems.");
#endif
        cfg.set_bool(CFG_COREDUMPS, true);
        return true;
    case CMD_GENERATE_KEY:
        cfg.set_bool(CFG_NEEDSSECKEY, true);
        *cmd = (optdefs_t) val;
        return true;
    case OPT_EXPERT:
        cfg.set_bool(CFG_EXPERT, true);
        return true;
    case CMD_LIST_KEYS:
    case CMD_EXPORT_KEY:
    case CMD_EXPORT_REV:
    case CMD_REVOKE_KEY:
    case CMD_REMOVE_KEY:
    case CMD_EDIT_KEY:
    case CMD_IMPORT:
    case CMD_IMPORT_KEYS:
    case CMD_IMPORT_SIGS:
    case CMD_HELP:
    case CMD_VERSION:
        *cmd = (optdefs_t) val;
        return true;
    /* options */
    case OPT_KEY_STORE_FORMAT:
        cfg.set_str(CFG_KEYSTOREFMT, arg);
        return true;
    case OPT_USERID:
        cfg.add_str(CFG_USERID, arg);
        return true;
    case OPT_HOMEDIR:
        cfg.set_str(CFG_HOMEDIR, arg);
        return true;
    case OPT_NUMBITS: {
        int bits = 0;
        if (!rnp::str_to_int(arg, bits) || (bits < 1024) || (bits > 16384)) {
            ERR_MSG("wrong bits value: %s", arg);
            return false;
        }
        cfg.set_int(CFG_NUMBITS, bits);
        return true;
    }
    case OPT_ALLOW_WEAK_HASH:
        cfg.set_bool(CFG_WEAK_HASH, true);
        return true;
    case OPT_ALLOW_SHA1:
        cfg.set_bool(CFG_ALLOW_SHA1, true);
        return true;
    case OPT_HASH_ALG:
        return cli_rnp_set_hash(cfg, arg);
    case OPT_S2K_ITER: {
        int iterations = atoi(arg);
        if (!iterations) {
            ERR_MSG("Wrong iterations value: %s", arg);
            return false;
        }
        cfg.set_int(CFG_S2K_ITER, iterations);
        return true;
    }
    case OPT_EXPIRATION:
        cfg.set_str(CFG_KG_PRIMARY_EXPIRATION, arg);
        cfg.set_str(CFG_KG_SUBKEY_EXPIRATION, arg);
        return true;
    case OPT_S2K_MSEC: {
        int msec = 0;
        if (!rnp::str_to_int(arg, msec) || !msec) {
            ERR_MSG("Invalid s2k msec value: %s", arg);
            return false;
        }
        cfg.set_int(CFG_S2K_MSEC, msec);
        return true;
    }
    case OPT_PASSWDFD:
        cfg.set_str(CFG_PASSFD, arg);
        return true;
    case OPT_PASSWD:
        cfg.set_str(CFG_PASSWD, arg);
        return true;
    case OPT_RESULTS:
        cfg.set_str(CFG_IO_RESS, arg);
        return true;
    case OPT_CIPHER:
        return cli_rnp_set_cipher(cfg, arg);
    case OPT_DEBUG:
        ERR_MSG("Option --debug is deprecated, ignoring.");
        return true;
    case OPT_OUTPUT:
        if (!arg) {
            ERR_MSG("No output filename argument provided");
            return false;
        }
        cfg.set_str(CFG_OUTFILE, arg);
        return true;
    case OPT_OVERWRITE:
        cfg.set_bool(CFG_OVERWRITE, true);
        return true;
    case OPT_FORCE:
        cfg.set_bool(CFG_FORCE, true);
        return true;
    case OPT_SECRET:
        cfg.set_bool(CFG_SECRET, true);
        return true;
    case OPT_WITH_SIGS:
        cfg.set_bool(CFG_WITH_SIGS, true);
        return true;
    case OPT_REV_TYPE: {
        std::string revtype = arg;
        if (revtype == "0") {
            revtype = "no";
        } else if (revtype == "1") {
            revtype = "superseded";
        } else if (revtype == "2") {
            revtype = "compromised";
        } else if (revtype == "3") {
            revtype = "retired";
        }
        cfg.set_str(CFG_REV_TYPE, revtype);
        return true;
    }
    case OPT_REV_REASON:
        cfg.set_str(CFG_REV_REASON, arg);
        return true;
    case OPT_PERMISSIVE:
        cfg.set_bool(CFG_PERMISSIVE, true);
        return true;
    case OPT_NOTTY:
        cfg.set_bool(CFG_NOTTY, true);
        return true;
    case OPT_FIX_25519_BITS:
        cfg.set_bool(CFG_FIX_25519_BITS, true);
        return true;
    case OPT_CHK_25519_BITS:
        cfg.set_bool(CFG_CHK_25519_BITS, true);
        return true;
    case OPT_CURTIME:
        cfg.set_str(CFG_CURTIME, arg);
        return true;
    case OPT_ALLOW_OLD_CIPHERS:
        cfg.set_bool(CFG_ALLOW_OLD_CIPHERS, true);
        return true;
    case OPT_ADD_SUBKEY:
        cfg.set_bool(CFG_ADD_SUBKEY, true);
        return true;
    case OPT_SET_EXPIRE:
        cfg.set_str(CFG_SET_KEY_EXPIRE, arg);
        return true;
    case OPT_KEYFILE:
        cfg.set_str(CFG_KEYFILE, arg);
        cfg.set_bool(CFG_KEYSTORE_DISABLED, true);
        return true;
    default:
        *cmd = CMD_HELP;
        return true;
    }
}

bool
rnpkeys_init(cli_rnp_t &rnp, const rnp_cfg &cfg)
{
    rnp_cfg rnpcfg;
    rnpcfg.load_defaults();
    rnpcfg.set_int(CFG_NUMBITS, DEFAULT_RSA_NUMBITS);
    rnpcfg.set_str(CFG_IO_RESS, "<stdout>");
    rnpcfg.copy(cfg);

    if (!cli_cfg_set_keystore_info(rnpcfg)) {
        ERR_MSG("fatal: cannot set keystore info");
        return false;
    }
    if (!rnp.init(rnpcfg)) {
        ERR_MSG("fatal: failed to initialize rnpkeys");
        return false;
    }
    if (!cli_rnp_check_weak_hash(&rnp)) {
        ERR_MSG("Weak hash algorithm detected. Pass --allow-weak-hash option if you really "
                "want to use it.");
        return false;
    }

    if (!cli_rnp_check_old_ciphers(&rnp)) {
        ERR_MSG("Old cipher detected. Pass --allow-old-ciphers option if you really "
                "want to use it.");
        return false;
    }

    bool disable_ks = rnp.cfg().get_bool(CFG_KEYSTORE_DISABLED);
    if (!disable_ks && !rnp.load_keyrings(true)) {
        ERR_MSG("fatal: failed to load keys");
        return false;
    }

    /* load the keyfile if any */
    if (disable_ks && !rnp.cfg().get_str(CFG_KEYFILE).empty() && !cli_rnp_add_key(&rnp)) {
        ERR_MSG("fatal: failed to load key(s) from the file");
        return false;
    }

    return true;
}
