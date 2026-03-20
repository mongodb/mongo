#ifndef RNPKEYS_H_
#define RNPKEYS_H_

#include <stdbool.h>
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#else
#include "uniwin.h"
#endif
#include "../rnp/fficli.h"
#include "logging.h"

#define DEFAULT_RSA_NUMBITS 3072

typedef enum {
    /* commands */
    CMD_NONE = 0,
    CMD_LIST_KEYS = 260,
    CMD_EXPORT_KEY,
    CMD_IMPORT,
    CMD_IMPORT_KEYS,
    CMD_IMPORT_SIGS,
    CMD_GENERATE_KEY,
    CMD_EXPORT_REV,
    CMD_REVOKE_KEY,
    CMD_REMOVE_KEY,
    CMD_EDIT_KEY,
    CMD_VERSION,
    CMD_HELP,

    /* options */
    OPT_KEY_STORE_FORMAT,
    OPT_USERID,
    OPT_HOMEDIR,
    OPT_NUMBITS,
    OPT_ALLOW_WEAK_HASH,
    OPT_ALLOW_SHA1,
    OPT_HASH_ALG,
    OPT_COREDUMPS,
    OPT_PASSWDFD,
    OPT_PASSWD,
    OPT_RESULTS,
    OPT_CIPHER,
    OPT_EXPERT,
    OPT_OUTPUT,
    OPT_OVERWRITE,
    OPT_FORCE,
    OPT_SECRET,
    OPT_S2K_ITER,
    OPT_S2K_MSEC,
    OPT_EXPIRATION,
    OPT_WITH_SIGS,
    OPT_REV_TYPE,
    OPT_REV_REASON,
    OPT_PERMISSIVE,
    OPT_NOTTY,
    OPT_FIX_25519_BITS,
    OPT_CHK_25519_BITS,
    OPT_CURTIME,
    OPT_ALLOW_OLD_CIPHERS,
    OPT_ADD_SUBKEY,
    OPT_SET_EXPIRE,
    OPT_KEYFILE,

    /* debug */
    OPT_DEBUG
} optdefs_t;

bool rnp_cmd(cli_rnp_t *rnp, optdefs_t cmd, const char *f);
bool setoption(rnp_cfg &cfg, optdefs_t *cmd, int val, const char *arg);
void print_usage(const char *usagemsg);

/**
 * @brief Initializes rnpkeys. Function allocates memory dynamically for
 *        rnp argument, which must be freed by the caller.
 *
 * @param rnp initialized rnp context
 * @param cfg configuration with settings from command line
 * @return true on success, or false otherwise.
 */
bool rnpkeys_init(cli_rnp_t &rnp, const rnp_cfg &cfg);

#endif /* _rnpkeys_ */
