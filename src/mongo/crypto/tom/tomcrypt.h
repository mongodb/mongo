/*    Copyright 2014 MongoDB Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#ifndef TOMCRYPT_H_
#define TOMCRYPT_H_
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* use configuration data */
#include "tomcrypt_custom.h"

#ifdef __cplusplus
extern "C" {
#endif

/* version */
#define CRYPT 0x0117
#define SCRYPT "1.17"

/* max size of either a cipher/hash block or symmetric key [largest of the two] */
#define MAXBLOCKSIZE 128

/* descriptor table size */
#define TAB_SIZE 32

/* error codes [will be expanded in future releases] */
enum {
    CRYPT_OK = 0, /* Result OK */
    CRYPT_ERROR,  /* Generic Error */
    CRYPT_NOP,    /* Not a failure but no operation was performed */

    CRYPT_INVALID_KEYSIZE, /* Invalid key size given */
    CRYPT_INVALID_ROUNDS,  /* Invalid number of rounds */
    CRYPT_FAIL_TESTVECTOR, /* Algorithm failed test vectors */

    CRYPT_BUFFER_OVERFLOW, /* Not enough space for output */
    CRYPT_INVALID_PACKET,  /* Invalid input packet given */

    CRYPT_INVALID_PRNGSIZE, /* Invalid number of bits for a PRNG */
    CRYPT_ERROR_READPRNG,   /* Could not read enough from PRNG */

    CRYPT_INVALID_CIPHER, /* Invalid cipher specified */
    CRYPT_INVALID_HASH,   /* Invalid hash specified */
    CRYPT_INVALID_PRNG,   /* Invalid PRNG specified */

    CRYPT_MEM, /* Out of memory */

    CRYPT_PK_TYPE_MISMATCH, /* Not equivalent types of PK keys */
    CRYPT_PK_NOT_PRIVATE,   /* Requires a private PK key */

    CRYPT_INVALID_ARG,   /* Generic invalid argument */
    CRYPT_FILE_NOTFOUND, /* File Not Found */

    CRYPT_PK_INVALID_TYPE,   /* Invalid type of PK key */
    CRYPT_PK_INVALID_SYSTEM, /* Invalid PK system specified */
    CRYPT_PK_DUP,            /* Duplicate key already in key ring */
    CRYPT_PK_NOT_FOUND,      /* Key not found in keyring */
    CRYPT_PK_INVALID_SIZE,   /* Invalid size input for PK parameters */

    CRYPT_INVALID_PRIME_SIZE, /* Invalid size of prime requested */
    CRYPT_PK_INVALID_PADDING  /* Invalid padding on input */
};

// clang-format off
#include "tomcrypt_cfg.h"
#include "tomcrypt_macros.h"
#include "tomcrypt_cipher.h"
#include "tomcrypt_hash.h"
#include "tomcrypt_mac.h"
//#include <tomcrypt_prng.h>
//#include <tomcrypt_pk.h>
//#include <tomcrypt_math.h>
#include "tomcrypt_misc.h"
#include "tomcrypt_argchk.h"
//#include <tomcrypt_pkcs.h>
// clang-format on

#ifdef __cplusplus
}
#endif

#endif /* TOMCRYPT_H_ */


/* $Source: /cvs/libtom/libtomcrypt/src/headers/tomcrypt.h,v $ */
/* $Revision: 1.21 $ */
/* $Date: 2006/12/16 19:34:05 $ */
