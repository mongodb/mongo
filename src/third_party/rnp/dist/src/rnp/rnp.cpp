/*
 * Copyright (c) 2017-2023 [Ribose Inc](https://www.ribose.com).
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
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#ifdef _MSC_VER
#include "uniwin.h"
#else
#include <sys/param.h>
#include <unistd.h>
#include <getopt.h>
#endif
#include <fcntl.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>
#include <errno.h>

#include "fficli.h"
#include "str-utils.h"
#include "logging.h"

static const char *usage =
  "Sign, verify, encrypt, decrypt, inspect OpenPGP data.\n"
  "Usage: rnp --command [options] [files]\n"
  "Commands:\n"
  "  -h, --help              This help message.\n"
  "  -V, --version           Print RNP version information.\n"
  "  -e, --encrypt           Encrypt data using the public key(s).\n"
  "    -r, --recipient       Specify recipient's key via uid/keyid/fingerprint.\n"
#if defined(ENABLE_CRYPTO_REFRESH)
  "    --v3-pkesk-only       Only create v3 PKESK (otherwise v6 will be created if "
  "appropriate).\n"
#endif
  "    --cipher name         Specify symmetric cipher, used for encryption.\n"
  "    --aead[=EAX, OCB]     Use AEAD for encryption.\n"
  "    -z 0..9               Set the compression level.\n"
  "    --[zip,zlib,bzip]     Use the corresponding compression algorithm.\n"
  "    --armor               Apply ASCII armor to the encryption/signing output.\n"
  "    --no-wrap             Do not wrap the output in a literal data packet.\n"
  "  -c, --symmetric         Encrypt data using the password(s).\n"
  "    --passwords num       Encrypt to the specified number of passwords.\n"
  "  -s, --sign              Sign data. May be combined with encryption.\n"
  "    --detach              Produce detached signature.\n"
  "    -u, --userid          Specify signing key(s) via uid/keyid/fingerprint.\n"
  "    --hash                Specify hash algorithm, used during signing.\n"
  "    --allow-weak-hash     Allow usage of a weak hash algorithm.\n"
  "    --allow-sha1-key-sigs Allow usage of a SHA-1 key signatures.\n"
  "  --clearsign             Cleartext-sign data.\n"
  "  -d, --decrypt           Decrypt and output data, verifying signatures.\n"
  "  -v, --verify            Verify signatures, without outputting data.\n"
  "    --source              Specify source for the detached signature.\n"
  "  --dearmor               Strip ASCII armor from the data, outputting binary.\n"
  "  --enarmor               Add ASCII armor to the data.\n"
  "  --list-packets          List OpenPGP packets from the input.\n"
  "    --json                Use JSON output instead of human-readable.\n"
  "    --grips               Dump key fingerprints and grips.\n"
  "    --mpi                 Dump MPI values from packets.\n"
  "    --raw                 Dump raw packet contents as well.\n"
  "\n"
  "Other options:\n"
  "  --homedir path          Override home directory (default is ~/.rnp/).\n"
  "  -f, --keyfile           Load key(s) only from the file specified.\n"
  "  --output [file, -]      Write data to the specified file or stdout.\n"
  "  --overwrite             Overwrite output file without a prompt.\n"
  "  --password              Password used during operation.\n"
  "  --pass-fd num           Read password(s) from the file descriptor.\n"
  "  --s2k-iterations        Set the number of iterations for the S2K process.\n"
  "  --s2k-msec              Calculate S2K iterations value based on a provided time in "
  "milliseconds.\n"
  "  --notty                 Do not output anything to the TTY.\n"
  "  --current-time          Override system's time.\n"
  "  --set-filename          Override file name, stored inside of OpenPGP message.\n"
  "\n"
  "See man page for a detailed listing and explanation.\n"
  "\n";

enum optdefs {
    /* Commands as they are get via CLI */
    CMD_ENCRYPT = 260,
    CMD_DECRYPT,
    CMD_SIGN,
    CMD_CLEARSIGN,
    CMD_VERIFY,
    CMD_VERIFY_CAT,
    CMD_SYM_ENCRYPT,
    CMD_DEARMOR,
    CMD_ENARMOR,
    CMD_LIST_PACKETS,
    CMD_VERSION,
    CMD_HELP,

    /* OpenPGP data processing commands. Sign/Encrypt/Decrypt mapped to these */
    CMD_PROTECT,
    CMD_PROCESS,

    /* Options */
    OPT_KEY_STORE_FORMAT,
    OPT_USERID,
    OPT_RECIPIENT,
#if defined(ENABLE_CRYPTO_REFRESH)
    OPT_V3_PKESK_ONLY,
#endif
    OPT_ARMOR,
    OPT_HOMEDIR,
    OPT_DETACHED,
    OPT_HASH_ALG,
    OPT_ALLOW_WEAK_HASH,
    OPT_ALLOW_SHA1,
    OPT_OUTPUT,
    OPT_RESULTS,
    OPT_COREDUMPS,
    OPT_PASSWDFD,
    OPT_PASSWD,
    OPT_PASSWORDS,
    OPT_EXPIRATION,
    OPT_CREATION,
    OPT_CIPHER,
    OPT_NUMTRIES,
    OPT_ZALG_ZIP,
    OPT_ZALG_ZLIB,
    OPT_ZALG_BZIP,
    OPT_ZLEVEL,
    OPT_OVERWRITE,
    OPT_AEAD,
    OPT_AEAD_CHUNK,
    OPT_KEYFILE,
    OPT_JSON,
    OPT_GRIPS,
    OPT_MPIS,
    OPT_RAW,
    OPT_NOTTY,
    OPT_SOURCE,
    OPT_NOWRAP,
    OPT_CURTIME,
    OPT_ALLOW_OLD_CIPHERS,
    OPT_SETFNAME,
    OPT_ALLOW_HIDDEN,
    OPT_S2K_ITER,
    OPT_S2K_MSEC,

    /* debug */
    OPT_DEBUG
};

#define EXIT_ERROR 2

static struct option options[] = {
  /* file manipulation commands */
  {"encrypt", no_argument, NULL, CMD_ENCRYPT},
  {"decrypt", no_argument, NULL, CMD_DECRYPT},
  {"sign", no_argument, NULL, CMD_SIGN},
  {"clearsign", no_argument, NULL, CMD_CLEARSIGN},
  {"verify", no_argument, NULL, CMD_VERIFY},
  {"verify-cat", no_argument, NULL, CMD_VERIFY_CAT},
  {"symmetric", no_argument, NULL, CMD_SYM_ENCRYPT},
  {"dearmor", no_argument, NULL, CMD_DEARMOR},
  {"enarmor", optional_argument, NULL, CMD_ENARMOR},
  /* file listing commands */
  {"list-packets", no_argument, NULL, CMD_LIST_PACKETS},
  /* debugging commands */
  {"help", no_argument, NULL, CMD_HELP},
  {"version", no_argument, NULL, CMD_VERSION},
  {"debug", required_argument, NULL, OPT_DEBUG},
  /* options */
  {"coredumps", no_argument, NULL, OPT_COREDUMPS},
  {"keystore-format", required_argument, NULL, OPT_KEY_STORE_FORMAT},
  {"userid", required_argument, NULL, OPT_USERID},
  {"recipient", required_argument, NULL, OPT_RECIPIENT},
#if defined(ENABLE_CRYPTO_REFRESH)
  {"v3-pkesk-only", optional_argument, NULL, OPT_V3_PKESK_ONLY},
#endif
  {"home", required_argument, NULL, OPT_HOMEDIR},
  {"homedir", required_argument, NULL, OPT_HOMEDIR},
  {"keyfile", required_argument, NULL, OPT_KEYFILE},
  {"ascii", no_argument, NULL, OPT_ARMOR},
  {"armor", no_argument, NULL, OPT_ARMOR},
  {"armour", no_argument, NULL, OPT_ARMOR},
  {"detach", no_argument, NULL, OPT_DETACHED},
  {"detached", no_argument, NULL, OPT_DETACHED},
  {"hash", required_argument, NULL, OPT_HASH_ALG},
  {"pass-fd", required_argument, NULL, OPT_PASSWDFD},
  {"password", required_argument, NULL, OPT_PASSWD},
  {"passwords", required_argument, NULL, OPT_PASSWORDS},
  {"output", required_argument, NULL, OPT_OUTPUT},
  {"results", required_argument, NULL, OPT_RESULTS},
  {"creation", required_argument, NULL, OPT_CREATION},
  {"expiration", required_argument, NULL, OPT_EXPIRATION},
  {"expiry", required_argument, NULL, OPT_EXPIRATION},
  {"cipher", required_argument, NULL, OPT_CIPHER},
  {"numtries", required_argument, NULL, OPT_NUMTRIES},
  {"zip", no_argument, NULL, OPT_ZALG_ZIP},
  {"zlib", no_argument, NULL, OPT_ZALG_ZLIB},
  {"bzip", no_argument, NULL, OPT_ZALG_BZIP},
  {"bzip2", no_argument, NULL, OPT_ZALG_BZIP},
  {"overwrite", no_argument, NULL, OPT_OVERWRITE},
  {"aead", optional_argument, NULL, OPT_AEAD},
  {"aead-chunk-bits", required_argument, NULL, OPT_AEAD_CHUNK},
  {"json", no_argument, NULL, OPT_JSON},
  {"grips", no_argument, NULL, OPT_GRIPS},
  {"mpi", no_argument, NULL, OPT_MPIS},
  {"raw", no_argument, NULL, OPT_RAW},
  {"notty", no_argument, NULL, OPT_NOTTY},
  {"source", required_argument, NULL, OPT_SOURCE},
  {"no-wrap", no_argument, NULL, OPT_NOWRAP},
  {"current-time", required_argument, NULL, OPT_CURTIME},
  {"allow-old-ciphers", no_argument, NULL, OPT_ALLOW_OLD_CIPHERS},
  {"set-filename", required_argument, NULL, OPT_SETFNAME},
  {"allow-hidden", no_argument, NULL, OPT_ALLOW_HIDDEN},
  {"s2k-iterations", required_argument, NULL, OPT_S2K_ITER},
  {"s2k-msec", required_argument, NULL, OPT_S2K_MSEC},
  {"allow-weak-hash", no_argument, NULL, OPT_ALLOW_WEAK_HASH},
  {"allow-sha1-key-sigs", no_argument, NULL, OPT_ALLOW_SHA1},

  {NULL, 0, NULL, 0},
};

/* print a usage message */
static void
print_usage(const char *usagemsg)
{
    cli_rnp_print_praise();
    puts(usagemsg);
}

/* do a command once for a specified config */
static bool
rnp_cmd(cli_rnp_t *rnp)
{
    bool ret = false;

    switch (rnp->cfg().get_int(CFG_COMMAND)) {
    case CMD_PROTECT:
        ret = cli_rnp_protect_file(rnp);
        break;
    case CMD_PROCESS:
        ret = cli_rnp_process_file(rnp);
        break;
    case CMD_LIST_PACKETS:
        ret = cli_rnp_dump_file(rnp);
        break;
    case CMD_DEARMOR:
        ret = cli_rnp_dearmor_file(rnp);
        break;
    case CMD_ENARMOR:
        ret = cli_rnp_armor_file(rnp);
        break;
    case CMD_VERSION:
        cli_rnp_print_praise();
        ret = true;
        break;
    default:
        print_usage(usage);
        ret = true;
    }

    return ret;
}

static bool
setcmd(rnp_cfg &cfg, int cmd, const char *arg)
{
    int newcmd = cmd;

    /* set file processing command to one of PROTECT or PROCESS */
    switch (cmd) {
    case CMD_ENCRYPT:
        cfg.set_bool(CFG_ENCRYPT_PK, true);
        if (cfg.get_bool(CFG_ENCRYPT_SK)) {
            cfg.set_bool(CFG_KEYSTORE_DISABLED, false);
        }
        newcmd = CMD_PROTECT;
        break;
    case CMD_SYM_ENCRYPT:
        cfg.set_bool(CFG_ENCRYPT_SK, true);
        if (!cfg.get_bool(CFG_ENCRYPT_PK) && !cfg.get_bool(CFG_SIGN_NEEDED)) {
            cfg.set_bool(CFG_KEYSTORE_DISABLED, true);
        }
        newcmd = CMD_PROTECT;
        break;
    case CMD_CLEARSIGN:
        cfg.set_bool(CFG_CLEARTEXT, true);
        FALLTHROUGH_STATEMENT;
    case CMD_SIGN:
        cfg.set_bool(CFG_NEEDSSECKEY, true);
        cfg.set_bool(CFG_SIGN_NEEDED, true);
        if (cfg.get_bool(CFG_ENCRYPT_SK)) {
            cfg.set_bool(CFG_KEYSTORE_DISABLED, false);
        }
        newcmd = CMD_PROTECT;
        break;
    case CMD_DECRYPT:
        /* for decryption, we probably need a seckey */
        cfg.set_bool(CFG_NEEDSSECKEY, true);
        newcmd = CMD_PROCESS;
        break;
    case CMD_VERIFY:
        /* single verify will discard output, decrypt will not */
        cfg.set_bool(CFG_NO_OUTPUT, true);
        FALLTHROUGH_STATEMENT;
    case CMD_VERIFY_CAT:
        newcmd = CMD_PROCESS;
        break;
    case CMD_LIST_PACKETS:
        cfg.set_bool(CFG_KEYSTORE_DISABLED, true);
        break;
    case CMD_DEARMOR:
        cfg.set_bool(CFG_KEYSTORE_DISABLED, true);
        break;
    case CMD_ENARMOR: {
        std::string msgt = arg ? arg : "";
        if (msgt.empty() || (msgt == "msg")) {
            msgt = "message";
        } else if (msgt == "pubkey") {
            msgt = "public key";
        } else if (msgt == "seckey") {
            msgt = "secret key";
        } else if (msgt == "sign") {
            msgt = "signature";
        } else {
            ERR_MSG("Wrong enarmor argument: %s", arg);
            return false;
        }

        if (!msgt.empty()) {
            cfg.set_str(CFG_ARMOR_DATA_TYPE, msgt);
        }
        cfg.set_bool(CFG_KEYSTORE_DISABLED, true);
        break;
    }
    case CMD_HELP:
    case CMD_VERSION:
        break;
    default:
        newcmd = CMD_HELP;
        break;
    }

    if (cfg.has(CFG_COMMAND) && cfg.get_int(CFG_COMMAND) != newcmd) {
        ERR_MSG("Conflicting commands!");
        return false;
    }

    cfg.set_int(CFG_COMMAND, newcmd);
    return true;
}

/* set an option */
static bool
setoption(rnp_cfg &cfg, int val, const char *arg)
{
    switch (val) {
    /* redirect commands to setcmd */
    case CMD_ENCRYPT:
    case CMD_SIGN:
    case CMD_CLEARSIGN:
    case CMD_DECRYPT:
    case CMD_SYM_ENCRYPT:
    case CMD_VERIFY:
    case CMD_VERIFY_CAT:
    case CMD_LIST_PACKETS:
    case CMD_DEARMOR:
    case CMD_ENARMOR:
    case CMD_HELP:
    case CMD_VERSION:
        return setcmd(cfg, val, arg);
    /* options */
    case OPT_COREDUMPS:
#ifdef _WIN32
        ERR_MSG("warning: --coredumps doesn't make sense on windows systems.");
#endif
        cfg.set_bool(CFG_COREDUMPS, true);
        return true;
    case OPT_KEY_STORE_FORMAT:
        cfg.set_str(CFG_KEYSTOREFMT, arg);
        return true;
    case OPT_USERID:
        cfg.add_str(CFG_SIGNERS, arg);
        return true;
    case OPT_RECIPIENT:
        cfg.add_str(CFG_RECIPIENTS, arg);
        return true;
#if defined(ENABLE_CRYPTO_REFRESH)
    case OPT_V3_PKESK_ONLY:
        cfg.set_bool(CFG_V3_PKESK_ONLY, true);
        return true;
#endif
    case OPT_ARMOR:
        cfg.set_bool(CFG_ARMOR, true);
        return true;
    case OPT_DETACHED:
        cfg.set_bool(CFG_DETACHED, true);
        return true;
    case OPT_HOMEDIR:
        cfg.set_str(CFG_HOMEDIR, arg);
        return true;
    case OPT_KEYFILE:
        cfg.set_str(CFG_KEYFILE, arg);
        cfg.set_bool(CFG_KEYSTORE_DISABLED, true);
        return true;
    case OPT_HASH_ALG:
        return cli_rnp_set_hash(cfg, arg);
    case OPT_ALLOW_WEAK_HASH:
        cfg.set_bool(CFG_WEAK_HASH, true);
        return true;
    case OPT_ALLOW_SHA1:
        cfg.set_bool(CFG_ALLOW_SHA1, true);
        return true;
    case OPT_PASSWDFD:
        cfg.set_str(CFG_PASSFD, arg);
        return true;
    case OPT_PASSWD:
        cfg.set_str(CFG_PASSWD, arg);
        return true;
    case OPT_PASSWORDS: {
        int count = 0;
        if (!rnp::str_to_int(arg, count) || (count <= 0)) {
            ERR_MSG("Incorrect value for --passwords option: %s", arg);
            return false;
        }

        cfg.set_int(CFG_PASSWORDC, count);
        cfg.set_bool(CFG_ENCRYPT_SK, true);
        return true;
    }
    case OPT_OUTPUT:
        cfg.set_str(CFG_OUTFILE, arg);
        return true;
    case OPT_RESULTS:
        cfg.set_str(CFG_RESULTS, arg);
        return true;
    case OPT_EXPIRATION:
        cfg.set_str(CFG_EXPIRATION, arg);
        return true;
    case OPT_CREATION:
        cfg.set_str(CFG_CREATION, arg);
        return true;
    case OPT_CIPHER:
        return cli_rnp_set_cipher(cfg, arg);
    case OPT_NUMTRIES:
        cfg.set_str(CFG_NUMTRIES, arg);
        return true;
    case OPT_ZALG_ZIP:
        cfg.set_str(CFG_ZALG, "ZIP");
        return true;
    case OPT_ZALG_ZLIB:
        cfg.set_str(CFG_ZALG, "ZLib");
        return true;
    case OPT_ZALG_BZIP:
        cfg.set_str(CFG_ZALG, "BZip2");
        return true;
    case OPT_AEAD: {
        std::string argstr = arg ? arg : "";
        if ((argstr == "1") || rnp::str_case_eq(argstr, "eax")) {
            argstr = "EAX";
        } else if (argstr.empty() || (argstr == "2") || rnp::str_case_eq(argstr, "ocb")) {
            argstr = "OCB";
        } else {
            ERR_MSG("Wrong AEAD algorithm: %s", argstr.c_str());
            return false;
        }
        cfg.set_str(CFG_AEAD, argstr);
        return true;
    }
    case OPT_AEAD_CHUNK: {
        int bits = 0;
        if (!rnp::str_to_int(arg, bits) || (bits < 0) || (bits > 16)) {
            ERR_MSG("Wrong argument value %s for aead-chunk-bits, must be 0..16.", arg);
            return false;
        }
        cfg.set_int(CFG_AEAD_CHUNK, bits);
        return true;
    }
    case OPT_OVERWRITE:
        cfg.set_bool(CFG_OVERWRITE, true);
        return true;
    case OPT_JSON:
        cfg.set_bool(CFG_JSON, true);
        return true;
    case OPT_GRIPS:
        cfg.set_bool(CFG_GRIPS, true);
        return true;
    case OPT_MPIS:
        cfg.set_bool(CFG_MPIS, true);
        return true;
    case OPT_RAW:
        cfg.set_bool(CFG_RAW, true);
        return true;
    case OPT_NOTTY:
        cfg.set_bool(CFG_NOTTY, true);
        return true;
    case OPT_SOURCE:
        cfg.set_str(CFG_SOURCE, arg);
        return true;
    case OPT_NOWRAP:
        cfg.set_bool(CFG_NOWRAP, true);
        cfg.set_int(CFG_ZLEVEL, 0);
        return true;
    case OPT_CURTIME:
        cfg.set_str(CFG_CURTIME, arg);
        return true;
    case OPT_ALLOW_OLD_CIPHERS:
        cfg.set_bool(CFG_ALLOW_OLD_CIPHERS, true);
        return true;
    case OPT_SETFNAME:
        cfg.set_str(CFG_SETFNAME, arg);
        return true;
    case OPT_ALLOW_HIDDEN:
        cfg.set_bool(CFG_ALLOW_HIDDEN, true);
        return true;
    case OPT_S2K_ITER: {
        int iterations = 0;
        if (!rnp::str_to_int(arg, iterations) || !iterations) {
            ERR_MSG("Wrong iterations value: %s", arg);
            return false;
        }
        cfg.set_int(CFG_S2K_ITER, iterations);
        return true;
    }
    case OPT_S2K_MSEC: {
        int msec = 0;
        if (!rnp::str_to_int(arg, msec) || !msec) {
            ERR_MSG("Invalid s2k msec value: %s", arg);
            return false;
        }
        cfg.set_int(CFG_S2K_MSEC, msec);
        return true;
    }
    case OPT_DEBUG:
        ERR_MSG("Option --debug is deprecated, ignoring.");
        return true;
    default:
        return setcmd(cfg, CMD_HELP, arg);
    }

    return false;
}

static bool
set_short_option(rnp_cfg &cfg, int ch, const char *arg)
{
    switch (ch) {
    case 'V':
        return setcmd(cfg, CMD_VERSION, arg);
    case 'd':
        return setcmd(cfg, CMD_DECRYPT, arg);
    case 'e':
        return setcmd(cfg, CMD_ENCRYPT, arg);
    case 'c':
        return setcmd(cfg, CMD_SYM_ENCRYPT, arg);
    case 's':
        return setcmd(cfg, CMD_SIGN, arg);
    case 'v':
        return setcmd(cfg, CMD_VERIFY, arg);
    case 'r':
        if (!strlen(optarg)) {
            ERR_MSG("Recipient should not be empty");
        } else {
            cfg.add_str(CFG_RECIPIENTS, optarg);
        }
        break;
    case 'u':
        if (!optarg) {
            ERR_MSG("No userid argument provided");
            return false;
        }
        cfg.add_str(CFG_SIGNERS, optarg);
        break;
    case 'z':
        if ((strlen(optarg) != 1) || (optarg[0] < '0') || (optarg[0] > '9')) {
            ERR_MSG("Bad compression level: %s. Should be 0..9", optarg);
        } else {
            cfg.set_int(CFG_ZLEVEL, optarg[0] - '0');
        }
        break;
    case 'f':
        if (!optarg) {
            ERR_MSG("No keyfile argument provided");
            return false;
        }
        cfg.set_str(CFG_KEYFILE, optarg);
        cfg.set_bool(CFG_KEYSTORE_DISABLED, true);
        break;
    case 'h':
        FALLTHROUGH_STATEMENT;
    default:
        return setcmd(cfg, CMD_HELP, optarg);
    }

    return true;
}

#ifndef RNP_RUN_TESTS
int
main(int argc, char **argv)
#else
int rnp_main(int argc, char **argv);
int
rnp_main(int argc, char **argv)
#endif
{
    if (argc < 2) {
        print_usage(usage);
        return EXIT_ERROR;
    }

    cli_rnp_t rnp = {};
#if !defined(RNP_RUN_TESTS) && defined(_WIN32)
    try {
        rnp.substitute_args(&argc, &argv);
    } catch (std::exception &ex) {
        RNP_LOG("Error converting arguments ('%s')", ex.what());
        return EXIT_ERROR;
    }
#endif

    rnp_cfg cfg;
    cfg.load_defaults();

    /* TODO: These options should be set after initialising the context. */
    int optindex = 0;
    int ch;
    while ((ch = getopt_long(argc, argv, "S:Vdecr:su:vz:f:h", options, &optindex)) != -1) {
        /* Check for unsupported command/option */
        if (ch == '?') {
            print_usage(usage);
            return EXIT_FAILURE;
        }

        bool res = ch >= CMD_ENCRYPT ? setoption(cfg, options[optindex].val, optarg) :
                                       set_short_option(cfg, ch, optarg);
        if (!res) {
            return EXIT_ERROR;
        }
    }

    switch (cfg.get_int(CFG_COMMAND)) {
    case CMD_HELP:
        print_usage(usage);
        return EXIT_SUCCESS;
    case CMD_VERSION:
        cli_rnp_print_praise();
        return EXIT_SUCCESS;
    default:;
    }

    if (!cli_cfg_set_keystore_info(cfg)) {
        ERR_MSG("fatal: cannot set keystore info");
        return EXIT_ERROR;
    }

    if (!rnp.init(cfg)) {
        ERR_MSG("fatal: cannot initialise");
        return EXIT_ERROR;
    }

    if (!cli_rnp_check_weak_hash(&rnp)) {
        ERR_MSG("Weak hash algorithm detected. Pass --allow-weak-hash option if you really "
                "want to use it.");
        return EXIT_ERROR;
    }

    if (!cli_rnp_check_old_ciphers(&rnp)) {
        ERR_MSG("Old cipher detected. Pass --allow-old-ciphers option if you really "
                "want to use it.");
        return EXIT_ERROR;
    }

    bool disable_ks = rnp.cfg().get_bool(CFG_KEYSTORE_DISABLED);
    if (!disable_ks && !rnp.load_keyrings(rnp.cfg().get_bool(CFG_NEEDSSECKEY))) {
        ERR_MSG("fatal: failed to load keys");
        return EXIT_ERROR;
    }

    /* load the keyfile if any */
    if (disable_ks && !rnp.cfg().get_str(CFG_KEYFILE).empty() && !cli_rnp_add_key(&rnp)) {
        ERR_MSG("fatal: failed to load key(s) from the file");
        return EXIT_ERROR;
    }

    if (!cli_rnp_setup(&rnp)) {
        return EXIT_ERROR;
    }

    /* now do the required action for each of the command line args */
    if (optind == argc) {
        return cli_rnp_t::ret_code(rnp_cmd(&rnp));
    }
    bool success = true;
    for (int i = optind; i < argc; i++) {
        rnp.cfg().set_str(CFG_INFILE, argv[i]);
        success = success && rnp_cmd(&rnp);
    }
    return cli_rnp_t::ret_code(success);
}
