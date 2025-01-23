/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/io/pkcs11.h>

#include "pkcs11_private.h"

#include <aws/common/mutex.h>
#include <aws/common/ref_count.h>
#include <aws/common/string.h>
#include <aws/io/logging.h>
#include <aws/io/shared_library.h>

#include <inttypes.h>

/* NOTE 1: even though we currently include the v2.40 headers, they're compatible with any v2.x library.
 * NOTE 2: v3.x is backwards compatible with 2.x, and even claims to be 2.40 if you check its version the 2.x way */
#define AWS_SUPPORTED_CRYPTOKI_VERSION_MAJOR 2
#define AWS_MIN_SUPPORTED_CRYPTOKI_VERSION_MINOR 20

/* clang-format off */
/*
 * DER encoded DigestInfo value to be prefixed to the hash, used for RSA signing
 * See https://tools.ietf.org/html/rfc3447#page-43
 * (Notes to help understand what's going on here with DER encoding)
 * 0x30 nn - Sequence of tags, nn bytes, including hash, nn = mm+jj+4 (PKCS11 DigestInfo)
 *   0x30 mm - Subsequence of tags, mm bytes (ii+4) (PKCS11
 *     0x06 ii - OID encoding, ii bytes, see X.680 - this identifies the hash algorithm
 *     0x05 00 - NULL
 *   0x04 jj - OCTET, nn = mm + jj + 4
 *   Digest (nn - mm - 4 bytes)
 */
static const uint8_t SHA1_PREFIX_TO_RSA_SIG[] = { 0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14 };
static const uint8_t SHA256_PREFIX_TO_RSA_SIG[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
static const uint8_t SHA384_PREFIX_TO_RSA_SIG[] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
static const uint8_t SHA512_PREFIX_TO_RSA_SIG[] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };
static const uint8_t SHA224_PREFIX_TO_RSA_SIG[] = { 0x30, 0x2d, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x04, 0x05, 0x00, 0x04, 0x1c };
/* clang-format on */

/* Return c-string for PKCS#11 CKR_* contants. */
const char *aws_pkcs11_ckr_str(CK_RV rv) {
    /* clang-format off */
    switch (rv) {
        case (CKR_OK): return "CKR_OK";
        case (CKR_CANCEL): return "CKR_CANCEL";
        case (CKR_HOST_MEMORY): return "CKR_HOST_MEMORY";
        case (CKR_SLOT_ID_INVALID): return "CKR_SLOT_ID_INVALID";
        case (CKR_GENERAL_ERROR): return "CKR_GENERAL_ERROR";
        case (CKR_FUNCTION_FAILED): return "CKR_FUNCTION_FAILED";
        case (CKR_ARGUMENTS_BAD): return "CKR_ARGUMENTS_BAD";
        case (CKR_NO_EVENT): return "CKR_NO_EVENT";
        case (CKR_NEED_TO_CREATE_THREADS): return "CKR_NEED_TO_CREATE_THREADS";
        case (CKR_CANT_LOCK): return "CKR_CANT_LOCK";
        case (CKR_ATTRIBUTE_READ_ONLY): return "CKR_ATTRIBUTE_READ_ONLY";
        case (CKR_ATTRIBUTE_SENSITIVE): return "CKR_ATTRIBUTE_SENSITIVE";
        case (CKR_ATTRIBUTE_TYPE_INVALID): return "CKR_ATTRIBUTE_TYPE_INVALID";
        case (CKR_ATTRIBUTE_VALUE_INVALID): return "CKR_ATTRIBUTE_VALUE_INVALID";
        case (CKR_ACTION_PROHIBITED): return "CKR_ACTION_PROHIBITED";
        case (CKR_DATA_INVALID): return "CKR_DATA_INVALID";
        case (CKR_DATA_LEN_RANGE): return "CKR_DATA_LEN_RANGE";
        case (CKR_DEVICE_ERROR): return "CKR_DEVICE_ERROR";
        case (CKR_DEVICE_MEMORY): return "CKR_DEVICE_MEMORY";
        case (CKR_DEVICE_REMOVED): return "CKR_DEVICE_REMOVED";
        case (CKR_ENCRYPTED_DATA_INVALID): return "CKR_ENCRYPTED_DATA_INVALID";
        case (CKR_ENCRYPTED_DATA_LEN_RANGE): return "CKR_ENCRYPTED_DATA_LEN_RANGE";
        case (CKR_FUNCTION_CANCELED): return "CKR_FUNCTION_CANCELED";
        case (CKR_FUNCTION_NOT_PARALLEL): return "CKR_FUNCTION_NOT_PARALLEL";
        case (CKR_FUNCTION_NOT_SUPPORTED): return "CKR_FUNCTION_NOT_SUPPORTED";
        case (CKR_KEY_HANDLE_INVALID): return "CKR_KEY_HANDLE_INVALID";
        case (CKR_KEY_SIZE_RANGE): return "CKR_KEY_SIZE_RANGE";
        case (CKR_KEY_TYPE_INCONSISTENT): return "CKR_KEY_TYPE_INCONSISTENT";
        case (CKR_KEY_NOT_NEEDED): return "CKR_KEY_NOT_NEEDED";
        case (CKR_KEY_CHANGED): return "CKR_KEY_CHANGED";
        case (CKR_KEY_NEEDED): return "CKR_KEY_NEEDED";
        case (CKR_KEY_INDIGESTIBLE): return "CKR_KEY_INDIGESTIBLE";
        case (CKR_KEY_FUNCTION_NOT_PERMITTED): return "CKR_KEY_FUNCTION_NOT_PERMITTED";
        case (CKR_KEY_NOT_WRAPPABLE): return "CKR_KEY_NOT_WRAPPABLE";
        case (CKR_KEY_UNEXTRACTABLE): return "CKR_KEY_UNEXTRACTABLE";
        case (CKR_MECHANISM_INVALID): return "CKR_MECHANISM_INVALID";
        case (CKR_MECHANISM_PARAM_INVALID): return "CKR_MECHANISM_PARAM_INVALID";
        case (CKR_OBJECT_HANDLE_INVALID): return "CKR_OBJECT_HANDLE_INVALID";
        case (CKR_OPERATION_ACTIVE): return "CKR_OPERATION_ACTIVE";
        case (CKR_OPERATION_NOT_INITIALIZED): return "CKR_OPERATION_NOT_INITIALIZED";
        case (CKR_PIN_INCORRECT): return "CKR_PIN_INCORRECT";
        case (CKR_PIN_INVALID): return "CKR_PIN_INVALID";
        case (CKR_PIN_LEN_RANGE): return "CKR_PIN_LEN_RANGE";
        case (CKR_PIN_EXPIRED): return "CKR_PIN_EXPIRED";
        case (CKR_PIN_LOCKED): return "CKR_PIN_LOCKED";
        case (CKR_SESSION_CLOSED): return "CKR_SESSION_CLOSED";
        case (CKR_SESSION_COUNT): return "CKR_SESSION_COUNT";
        case (CKR_SESSION_HANDLE_INVALID): return "CKR_SESSION_HANDLE_INVALID";
        case (CKR_SESSION_PARALLEL_NOT_SUPPORTED): return "CKR_SESSION_PARALLEL_NOT_SUPPORTED";
        case (CKR_SESSION_READ_ONLY): return "CKR_SESSION_READ_ONLY";
        case (CKR_SESSION_EXISTS): return "CKR_SESSION_EXISTS";
        case (CKR_SESSION_READ_ONLY_EXISTS): return "CKR_SESSION_READ_ONLY_EXISTS";
        case (CKR_SESSION_READ_WRITE_SO_EXISTS): return "CKR_SESSION_READ_WRITE_SO_EXISTS";
        case (CKR_SIGNATURE_INVALID): return "CKR_SIGNATURE_INVALID";
        case (CKR_SIGNATURE_LEN_RANGE): return "CKR_SIGNATURE_LEN_RANGE";
        case (CKR_TEMPLATE_INCOMPLETE): return "CKR_TEMPLATE_INCOMPLETE";
        case (CKR_TEMPLATE_INCONSISTENT): return "CKR_TEMPLATE_INCONSISTENT";
        case (CKR_TOKEN_NOT_PRESENT): return "CKR_TOKEN_NOT_PRESENT";
        case (CKR_TOKEN_NOT_RECOGNIZED): return "CKR_TOKEN_NOT_RECOGNIZED";
        case (CKR_TOKEN_WRITE_PROTECTED): return "CKR_TOKEN_WRITE_PROTECTED";
        case (CKR_UNWRAPPING_KEY_HANDLE_INVALID): return "CKR_UNWRAPPING_KEY_HANDLE_INVALID";
        case (CKR_UNWRAPPING_KEY_SIZE_RANGE): return "CKR_UNWRAPPING_KEY_SIZE_RANGE";
        case (CKR_UNWRAPPING_KEY_TYPE_INCONSISTENT): return "CKR_UNWRAPPING_KEY_TYPE_INCONSISTENT";
        case (CKR_USER_ALREADY_LOGGED_IN): return "CKR_USER_ALREADY_LOGGED_IN";
        case (CKR_USER_NOT_LOGGED_IN): return "CKR_USER_NOT_LOGGED_IN";
        case (CKR_USER_PIN_NOT_INITIALIZED): return "CKR_USER_PIN_NOT_INITIALIZED";
        case (CKR_USER_TYPE_INVALID): return "CKR_USER_TYPE_INVALID";
        case (CKR_USER_ANOTHER_ALREADY_LOGGED_IN): return "CKR_USER_ANOTHER_ALREADY_LOGGED_IN";
        case (CKR_USER_TOO_MANY_TYPES): return "CKR_USER_TOO_MANY_TYPES";
        case (CKR_WRAPPED_KEY_INVALID): return "CKR_WRAPPED_KEY_INVALID";
        case (CKR_WRAPPED_KEY_LEN_RANGE): return "CKR_WRAPPED_KEY_LEN_RANGE";
        case (CKR_WRAPPING_KEY_HANDLE_INVALID): return "CKR_WRAPPING_KEY_HANDLE_INVALID";
        case (CKR_WRAPPING_KEY_SIZE_RANGE): return "CKR_WRAPPING_KEY_SIZE_RANGE";
        case (CKR_WRAPPING_KEY_TYPE_INCONSISTENT): return "CKR_WRAPPING_KEY_TYPE_INCONSISTENT";
        case (CKR_RANDOM_SEED_NOT_SUPPORTED): return "CKR_RANDOM_SEED_NOT_SUPPORTED";
        case (CKR_RANDOM_NO_RNG): return "CKR_RANDOM_NO_RNG";
        case (CKR_DOMAIN_PARAMS_INVALID): return "CKR_DOMAIN_PARAMS_INVALID";
        case (CKR_CURVE_NOT_SUPPORTED): return "CKR_CURVE_NOT_SUPPORTED";
        case (CKR_BUFFER_TOO_SMALL): return "CKR_BUFFER_TOO_SMALL";
        case (CKR_SAVED_STATE_INVALID): return "CKR_SAVED_STATE_INVALID";
        case (CKR_INFORMATION_SENSITIVE): return "CKR_INFORMATION_SENSITIVE";
        case (CKR_STATE_UNSAVEABLE): return "CKR_STATE_UNSAVEABLE";
        case (CKR_CRYPTOKI_NOT_INITIALIZED): return "CKR_CRYPTOKI_NOT_INITIALIZED";
        case (CKR_CRYPTOKI_ALREADY_INITIALIZED): return "CKR_CRYPTOKI_ALREADY_INITIALIZED";
        case (CKR_MUTEX_BAD): return "CKR_MUTEX_BAD";
        case (CKR_MUTEX_NOT_LOCKED): return "CKR_MUTEX_NOT_LOCKED";
        case (CKR_NEW_PIN_MODE): return "CKR_NEW_PIN_MODE";
        case (CKR_NEXT_OTP): return "CKR_NEXT_OTP";
        case (CKR_EXCEEDED_MAX_ITERATIONS): return "CKR_EXCEEDED_MAX_ITERATIONS";
        case (CKR_FIPS_SELF_TEST_FAILED): return "CKR_FIPS_SELF_TEST_FAILED";
        case (CKR_LIBRARY_LOAD_FAILED): return "CKR_LIBRARY_LOAD_FAILED";
        case (CKR_PIN_TOO_WEAK): return "CKR_PIN_TOO_WEAK";
        case (CKR_PUBLIC_KEY_INVALID): return "CKR_PUBLIC_KEY_INVALID";
        case (CKR_FUNCTION_REJECTED): return "CKR_FUNCTION_REJECTED";
        default: return "<UNKNOWN RETURN VALUE>";
    }
    /* clang-format on */
}

/* Translate from a CK_RV to an AWS error code */
static int s_ck_to_aws_error(CK_RV rv) {
    AWS_ASSERT(rv != CKR_OK);
    /* clang-format off */
    switch (rv) {
        case (CKR_CANCEL): return AWS_ERROR_PKCS11_CKR_CANCEL;
        case (CKR_HOST_MEMORY): return AWS_ERROR_PKCS11_CKR_HOST_MEMORY;
        case (CKR_SLOT_ID_INVALID): return AWS_ERROR_PKCS11_CKR_SLOT_ID_INVALID;
        case (CKR_GENERAL_ERROR): return AWS_ERROR_PKCS11_CKR_GENERAL_ERROR;
        case (CKR_FUNCTION_FAILED): return AWS_ERROR_PKCS11_CKR_FUNCTION_FAILED;
        case (CKR_ARGUMENTS_BAD): return AWS_ERROR_PKCS11_CKR_ARGUMENTS_BAD;
        case (CKR_NO_EVENT): return AWS_ERROR_PKCS11_CKR_NO_EVENT;
        case (CKR_NEED_TO_CREATE_THREADS): return AWS_ERROR_PKCS11_CKR_NEED_TO_CREATE_THREADS;
        case (CKR_CANT_LOCK): return AWS_ERROR_PKCS11_CKR_CANT_LOCK;
        case (CKR_ATTRIBUTE_READ_ONLY): return AWS_ERROR_PKCS11_CKR_ATTRIBUTE_READ_ONLY;
        case (CKR_ATTRIBUTE_SENSITIVE): return AWS_ERROR_PKCS11_CKR_ATTRIBUTE_SENSITIVE;
        case (CKR_ATTRIBUTE_TYPE_INVALID): return AWS_ERROR_PKCS11_CKR_ATTRIBUTE_TYPE_INVALID;
        case (CKR_ATTRIBUTE_VALUE_INVALID): return AWS_ERROR_PKCS11_CKR_ATTRIBUTE_VALUE_INVALID;
        case (CKR_ACTION_PROHIBITED): return AWS_ERROR_PKCS11_CKR_ACTION_PROHIBITED;
        case (CKR_DATA_INVALID): return AWS_ERROR_PKCS11_CKR_DATA_INVALID;
        case (CKR_DATA_LEN_RANGE): return AWS_ERROR_PKCS11_CKR_DATA_LEN_RANGE;
        case (CKR_DEVICE_ERROR): return AWS_ERROR_PKCS11_CKR_DEVICE_ERROR;
        case (CKR_DEVICE_MEMORY): return AWS_ERROR_PKCS11_CKR_DEVICE_MEMORY;
        case (CKR_DEVICE_REMOVED): return AWS_ERROR_PKCS11_CKR_DEVICE_REMOVED;
        case (CKR_ENCRYPTED_DATA_INVALID): return AWS_ERROR_PKCS11_CKR_ENCRYPTED_DATA_INVALID;
        case (CKR_ENCRYPTED_DATA_LEN_RANGE): return AWS_ERROR_PKCS11_CKR_ENCRYPTED_DATA_LEN_RANGE;
        case (CKR_FUNCTION_CANCELED): return AWS_ERROR_PKCS11_CKR_FUNCTION_CANCELED;
        case (CKR_FUNCTION_NOT_PARALLEL): return AWS_ERROR_PKCS11_CKR_FUNCTION_NOT_PARALLEL;
        case (CKR_FUNCTION_NOT_SUPPORTED): return AWS_ERROR_PKCS11_CKR_FUNCTION_NOT_SUPPORTED;
        case (CKR_KEY_HANDLE_INVALID): return AWS_ERROR_PKCS11_CKR_KEY_HANDLE_INVALID;
        case (CKR_KEY_SIZE_RANGE): return AWS_ERROR_PKCS11_CKR_KEY_SIZE_RANGE;
        case (CKR_KEY_TYPE_INCONSISTENT): return AWS_ERROR_PKCS11_CKR_KEY_TYPE_INCONSISTENT;
        case (CKR_KEY_NOT_NEEDED): return AWS_ERROR_PKCS11_CKR_KEY_NOT_NEEDED;
        case (CKR_KEY_CHANGED): return AWS_ERROR_PKCS11_CKR_KEY_CHANGED;
        case (CKR_KEY_NEEDED): return AWS_ERROR_PKCS11_CKR_KEY_NEEDED;
        case (CKR_KEY_INDIGESTIBLE): return AWS_ERROR_PKCS11_CKR_KEY_INDIGESTIBLE;
        case (CKR_KEY_FUNCTION_NOT_PERMITTED): return AWS_ERROR_PKCS11_CKR_KEY_FUNCTION_NOT_PERMITTED;
        case (CKR_KEY_NOT_WRAPPABLE): return AWS_ERROR_PKCS11_CKR_KEY_NOT_WRAPPABLE;
        case (CKR_KEY_UNEXTRACTABLE): return AWS_ERROR_PKCS11_CKR_KEY_UNEXTRACTABLE;
        case (CKR_MECHANISM_INVALID): return AWS_ERROR_PKCS11_CKR_MECHANISM_INVALID;
        case (CKR_MECHANISM_PARAM_INVALID): return AWS_ERROR_PKCS11_CKR_MECHANISM_PARAM_INVALID;
        case (CKR_OBJECT_HANDLE_INVALID): return AWS_ERROR_PKCS11_CKR_OBJECT_HANDLE_INVALID;
        case (CKR_OPERATION_ACTIVE): return AWS_ERROR_PKCS11_CKR_OPERATION_ACTIVE;
        case (CKR_OPERATION_NOT_INITIALIZED): return AWS_ERROR_PKCS11_CKR_OPERATION_NOT_INITIALIZED;
        case (CKR_PIN_INCORRECT): return AWS_ERROR_PKCS11_CKR_PIN_INCORRECT;
        case (CKR_PIN_INVALID): return AWS_ERROR_PKCS11_CKR_PIN_INVALID;
        case (CKR_PIN_LEN_RANGE): return AWS_ERROR_PKCS11_CKR_PIN_LEN_RANGE;
        case (CKR_PIN_EXPIRED): return AWS_ERROR_PKCS11_CKR_PIN_EXPIRED;
        case (CKR_PIN_LOCKED): return AWS_ERROR_PKCS11_CKR_PIN_LOCKED;
        case (CKR_SESSION_CLOSED): return AWS_ERROR_PKCS11_CKR_SESSION_CLOSED;
        case (CKR_SESSION_COUNT): return AWS_ERROR_PKCS11_CKR_SESSION_COUNT;
        case (CKR_SESSION_HANDLE_INVALID): return AWS_ERROR_PKCS11_CKR_SESSION_HANDLE_INVALID;
        case (CKR_SESSION_PARALLEL_NOT_SUPPORTED): return AWS_ERROR_PKCS11_CKR_SESSION_PARALLEL_NOT_SUPPORTED;
        case (CKR_SESSION_READ_ONLY): return AWS_ERROR_PKCS11_CKR_SESSION_READ_ONLY;
        case (CKR_SESSION_EXISTS): return AWS_ERROR_PKCS11_CKR_SESSION_EXISTS;
        case (CKR_SESSION_READ_ONLY_EXISTS): return AWS_ERROR_PKCS11_CKR_SESSION_READ_ONLY_EXISTS;
        case (CKR_SESSION_READ_WRITE_SO_EXISTS): return AWS_ERROR_PKCS11_CKR_SESSION_READ_WRITE_SO_EXISTS;
        case (CKR_SIGNATURE_INVALID): return AWS_ERROR_PKCS11_CKR_SIGNATURE_INVALID;
        case (CKR_SIGNATURE_LEN_RANGE): return AWS_ERROR_PKCS11_CKR_SIGNATURE_LEN_RANGE;
        case (CKR_TEMPLATE_INCOMPLETE): return AWS_ERROR_PKCS11_CKR_TEMPLATE_INCOMPLETE;
        case (CKR_TEMPLATE_INCONSISTENT): return AWS_ERROR_PKCS11_CKR_TEMPLATE_INCONSISTENT;
        case (CKR_TOKEN_NOT_PRESENT): return AWS_ERROR_PKCS11_CKR_TOKEN_NOT_PRESENT;
        case (CKR_TOKEN_NOT_RECOGNIZED): return AWS_ERROR_PKCS11_CKR_TOKEN_NOT_RECOGNIZED;
        case (CKR_TOKEN_WRITE_PROTECTED): return AWS_ERROR_PKCS11_CKR_TOKEN_WRITE_PROTECTED;
        case (CKR_UNWRAPPING_KEY_HANDLE_INVALID): return AWS_ERROR_PKCS11_CKR_UNWRAPPING_KEY_HANDLE_INVALID;
        case (CKR_UNWRAPPING_KEY_SIZE_RANGE): return AWS_ERROR_PKCS11_CKR_UNWRAPPING_KEY_SIZE_RANGE;
        case (CKR_UNWRAPPING_KEY_TYPE_INCONSISTENT): return AWS_ERROR_PKCS11_CKR_UNWRAPPING_KEY_TYPE_INCONSISTENT;
        case (CKR_USER_ALREADY_LOGGED_IN): return AWS_ERROR_PKCS11_CKR_USER_ALREADY_LOGGED_IN;
        case (CKR_USER_NOT_LOGGED_IN): return AWS_ERROR_PKCS11_CKR_USER_NOT_LOGGED_IN;
        case (CKR_USER_PIN_NOT_INITIALIZED): return AWS_ERROR_PKCS11_CKR_USER_PIN_NOT_INITIALIZED;
        case (CKR_USER_TYPE_INVALID): return AWS_ERROR_PKCS11_CKR_USER_TYPE_INVALID;
        case (CKR_USER_ANOTHER_ALREADY_LOGGED_IN): return AWS_ERROR_PKCS11_CKR_USER_ANOTHER_ALREADY_LOGGED_IN;
        case (CKR_USER_TOO_MANY_TYPES): return AWS_ERROR_PKCS11_CKR_USER_TOO_MANY_TYPES;
        case (CKR_WRAPPED_KEY_INVALID): return AWS_ERROR_PKCS11_CKR_WRAPPED_KEY_INVALID;
        case (CKR_WRAPPED_KEY_LEN_RANGE): return AWS_ERROR_PKCS11_CKR_WRAPPED_KEY_LEN_RANGE;
        case (CKR_WRAPPING_KEY_HANDLE_INVALID): return AWS_ERROR_PKCS11_CKR_WRAPPING_KEY_HANDLE_INVALID;
        case (CKR_WRAPPING_KEY_SIZE_RANGE): return AWS_ERROR_PKCS11_CKR_WRAPPING_KEY_SIZE_RANGE;
        case (CKR_WRAPPING_KEY_TYPE_INCONSISTENT): return AWS_ERROR_PKCS11_CKR_WRAPPING_KEY_TYPE_INCONSISTENT;
        case (CKR_RANDOM_SEED_NOT_SUPPORTED): return AWS_ERROR_PKCS11_CKR_RANDOM_SEED_NOT_SUPPORTED;
        case (CKR_RANDOM_NO_RNG): return AWS_ERROR_PKCS11_CKR_RANDOM_NO_RNG;
        case (CKR_DOMAIN_PARAMS_INVALID): return AWS_ERROR_PKCS11_CKR_DOMAIN_PARAMS_INVALID;
        case (CKR_CURVE_NOT_SUPPORTED): return AWS_ERROR_PKCS11_CKR_CURVE_NOT_SUPPORTED;
        case (CKR_BUFFER_TOO_SMALL): return AWS_ERROR_PKCS11_CKR_BUFFER_TOO_SMALL;
        case (CKR_SAVED_STATE_INVALID): return AWS_ERROR_PKCS11_CKR_SAVED_STATE_INVALID;
        case (CKR_INFORMATION_SENSITIVE): return AWS_ERROR_PKCS11_CKR_INFORMATION_SENSITIVE;
        case (CKR_STATE_UNSAVEABLE): return AWS_ERROR_PKCS11_CKR_STATE_UNSAVEABLE;
        case (CKR_CRYPTOKI_NOT_INITIALIZED): return AWS_ERROR_PKCS11_CKR_CRYPTOKI_NOT_INITIALIZED;
        case (CKR_CRYPTOKI_ALREADY_INITIALIZED): return AWS_ERROR_PKCS11_CKR_CRYPTOKI_ALREADY_INITIALIZED;
        case (CKR_MUTEX_BAD): return AWS_ERROR_PKCS11_CKR_MUTEX_BAD;
        case (CKR_MUTEX_NOT_LOCKED): return AWS_ERROR_PKCS11_CKR_MUTEX_NOT_LOCKED;
        case (CKR_NEW_PIN_MODE): return AWS_ERROR_PKCS11_CKR_NEW_PIN_MODE;
        case (CKR_NEXT_OTP): return AWS_ERROR_PKCS11_CKR_NEXT_OTP;
        case (CKR_EXCEEDED_MAX_ITERATIONS): return AWS_ERROR_PKCS11_CKR_EXCEEDED_MAX_ITERATIONS;
        case (CKR_FIPS_SELF_TEST_FAILED): return AWS_ERROR_PKCS11_CKR_FIPS_SELF_TEST_FAILED;
        case (CKR_LIBRARY_LOAD_FAILED): return AWS_ERROR_PKCS11_CKR_LIBRARY_LOAD_FAILED;
        case (CKR_PIN_TOO_WEAK): return AWS_ERROR_PKCS11_CKR_PIN_TOO_WEAK;
        case (CKR_PUBLIC_KEY_INVALID): return AWS_ERROR_PKCS11_CKR_PUBLIC_KEY_INVALID;
        case (CKR_FUNCTION_REJECTED): return AWS_ERROR_PKCS11_CKR_FUNCTION_REJECTED;
        default: return AWS_ERROR_PKCS11_UNKNOWN_CRYPTOKI_RETURN_VALUE;
    }
    /* clang-format on */
}

/* Return c-string for PKCS#11 CKK_* contants. */
static const char *s_ckk_str(CK_KEY_TYPE key_type) {
    /* clang-format off */
    switch(key_type) {
        case (CKK_RSA): return "CKK_RSA";
        case (CKK_DSA): return "CKK_DSA";
        case (CKK_DH): return "CKK_DH";
        case (CKK_EC): return "CKK_EC";
        case (CKK_X9_42_DH): return "CKK_X9_42_DH";
        case (CKK_KEA): return "CKK_KEA";
        case (CKK_GENERIC_SECRET): return "CKK_GENERIC_SECRET";
        case (CKK_RC2): return "CKK_RC2";
        case (CKK_RC4): return "CKK_RC4";
        case (CKK_DES): return "CKK_DES";
        case (CKK_DES2): return "CKK_DES2";
        case (CKK_DES3): return "CKK_DES3";
        case (CKK_CAST): return "CKK_CAST";
        case (CKK_CAST3): return "CKK_CAST3";
        case (CKK_CAST128): return "CKK_CAST128";
        case (CKK_RC5): return "CKK_RC5";
        case (CKK_IDEA): return "CKK_IDEA";
        case (CKK_SKIPJACK): return "CKK_SKIPJACK";
        case (CKK_BATON): return "CKK_BATON";
        case (CKK_JUNIPER): return "CKK_JUNIPER";
        case (CKK_CDMF): return "CKK_CDMF";
        case (CKK_AES): return "CKK_AES";
        case (CKK_BLOWFISH): return "CKK_BLOWFISH";
        case (CKK_TWOFISH): return "CKK_TWOFISH";
        case (CKK_SECURID): return "CKK_SECURID";
        case (CKK_HOTP): return "CKK_HOTP";
        case (CKK_ACTI): return "CKK_ACTI";
        case (CKK_CAMELLIA): return "CKK_CAMELLIA";
        case (CKK_ARIA): return "CKK_ARIA";
        case (CKK_MD5_HMAC): return "CKK_MD5_HMAC";
        case (CKK_SHA_1_HMAC): return "CKK_SHA_1_HMAC";
        case (CKK_RIPEMD128_HMAC): return "CKK_RIPEMD128_HMAC";
        case (CKK_RIPEMD160_HMAC): return "CKK_RIPEMD160_HMAC";
        case (CKK_SHA256_HMAC): return "CKK_SHA256_HMAC";
        case (CKK_SHA384_HMAC): return "CKK_SHA384_HMAC";
        case (CKK_SHA512_HMAC): return "CKK_SHA512_HMAC";
        case (CKK_SHA224_HMAC): return "CKK_SHA224_HMAC";
        case (CKK_SEED): return "CKK_SEED";
        case (CKK_GOSTR3410): return "CKK_GOSTR3410";
        case (CKK_GOSTR3411): return "CKK_GOSTR3411";
        case (CKK_GOST28147): return "CKK_GOST28147";
        default: return "<UNKNOWN KEY TYPE>";
    }
    /* clang-format on */
}

/* Log the failure of a PKCS#11 function, and call aws_raise_error() with the appropriate AWS error code */
static int s_raise_ck_error(const struct aws_pkcs11_lib *pkcs11_lib, const char *fn_name, CK_RV rv) {
    int aws_err = s_ck_to_aws_error(rv);

    AWS_LOGF_ERROR(
        AWS_LS_IO_PKCS11,
        "id=%p: %s() failed. PKCS#11 error: %s (0x%08lX). AWS error: %s",
        (void *)pkcs11_lib,
        fn_name,
        aws_pkcs11_ckr_str(rv),
        rv,
        aws_error_name(aws_err));

    return aws_raise_error(aws_err);
}

/* Log the failure of a PKCS#11 session-handle function and call aws_raise_error() with the appropriate error code */
static int s_raise_ck_session_error(
    const struct aws_pkcs11_lib *pkcs11_lib,
    const char *fn_name,
    CK_SESSION_HANDLE session,
    CK_RV rv) {

    int aws_err = s_ck_to_aws_error(rv);

    AWS_LOGF_ERROR(
        AWS_LS_IO_PKCS11,
        "id=%p session=%lu: %s() failed. PKCS#11 error: %s (0x%08lX). AWS error: %s",
        (void *)pkcs11_lib,
        session,
        fn_name,
        aws_pkcs11_ckr_str(rv),
        rv,
        aws_error_name(aws_err));

    return aws_raise_error(aws_err);
}

/* PKCS#11 often pads strings with ' ' */
static bool s_is_padding(uint8_t c) {
    return c == ' ';
}

/* Return byte-cursor to string with ' ' padding trimmed off.
 * PKCS#11 structs commonly stores strings in fixed-width arrays, padded by ' ' instead of null-terminator */
static struct aws_byte_cursor s_trim_padding(const uint8_t *str, size_t len) {
    const struct aws_byte_cursor src = aws_byte_cursor_from_array(str, len);
    return aws_byte_cursor_right_trim_pred(&src, s_is_padding);
}

/* Callback for PKCS#11 library to create a mutex.
 * Described in PKCS11-base-v2.40 section 3.7 */
static CK_RV s_pkcs11_create_mutex(CK_VOID_PTR_PTR mutex_out) {
    if (mutex_out == NULL) {
        return CKR_GENERAL_ERROR;
    }

    /* Using the default allocator because there's no way to know which PKCS#11 instance is invoking this callback */
    struct aws_allocator *allocator = aws_default_allocator();

    struct aws_mutex *mutex = aws_mem_calloc(allocator, 1, sizeof(struct aws_mutex));
    if (aws_mutex_init(mutex)) {
        AWS_LOGF_ERROR(AWS_LS_IO_PKCS11, "PKCS#11 CreateMutex() failed, error %s", aws_error_name(aws_last_error()));
        aws_mem_release(allocator, mutex);
        *mutex_out = NULL;
        return CKR_GENERAL_ERROR;
    }

    *mutex_out = mutex;
    return CKR_OK;
}

/* Callback for PKCS#11 library to destroy a mutex.
 * Described in PKCS11-base-v2.40 section 3.7 */
static CK_RV s_pkcs11_destroy_mutex(CK_VOID_PTR mutex_ptr) {
    if (mutex_ptr == NULL) {
        return CKR_GENERAL_ERROR;
    }

    struct aws_mutex *mutex = mutex_ptr;
    aws_mutex_clean_up(mutex);
    aws_mem_release(aws_default_allocator(), mutex);
    return CKR_OK;
}

/* Callback for PKCS#11 library to lock a mutex.
 * Described in PKCS11-base-v2.40 section 3.7 */
static CK_RV s_pkcs11_lock_mutex(CK_VOID_PTR mutex_ptr) {
    if (mutex_ptr == NULL) {
        return CKR_GENERAL_ERROR;
    }

    struct aws_mutex *mutex = mutex_ptr;
    if (aws_mutex_lock(mutex)) {
        AWS_LOGF_ERROR(AWS_LS_IO_PKCS11, "PKCS#11 LockMutex() failed, error %s", aws_error_name(aws_last_error()));
        return CKR_GENERAL_ERROR;
    }

    return CKR_OK;
}

/* Callback for PKCS#11 library to unlock a mutex.
 * Described in PKCS11-base-v2.40 section 3.7 */
static CK_RV s_pkcs11_unlock_mutex(CK_VOID_PTR mutex_ptr) {
    if (mutex_ptr == NULL) {
        return CKR_GENERAL_ERROR;
    }

    struct aws_mutex *mutex = mutex_ptr;
    if (aws_mutex_unlock(mutex)) {
        AWS_LOGF_ERROR(AWS_LS_IO_PKCS11, "PKCS#11 LockMutex() failed, error %s", aws_error_name(aws_last_error()));

        /* NOTE: Cryptoki has a CKR_MUTEX_NOT_LOCKED error code.
         * But posix doesn't treat this as an error and neither does windows so ¯\_(ツ)_/¯
         * If aws_mutex_unlock() failed here, it was something else. */
        return CKR_GENERAL_ERROR;
    }

    return CKR_OK;
}

struct aws_pkcs11_lib {
    struct aws_ref_count ref_count;
    struct aws_allocator *allocator;

    struct aws_shared_library shared_lib;

    CK_FUNCTION_LIST_PTR function_list;

    /* If true, C_Finalize() should be called when last ref-count is released */
    bool finalize_on_cleanup;
};

/* Invoked when last ref-count is released. Free all resources.
 * Note that this is also called if initialization fails half-way through */
static void s_pkcs11_lib_destroy(void *user_data) {
    struct aws_pkcs11_lib *pkcs11_lib = user_data;

    AWS_LOGF_DEBUG(
        AWS_LS_IO_PKCS11,
        "id=%p: Unloading PKCS#11. C_Finalize:%s",
        (void *)pkcs11_lib,
        pkcs11_lib->finalize_on_cleanup ? "yes" : "omit");

    if (pkcs11_lib->finalize_on_cleanup) {
        CK_RV rv = pkcs11_lib->function_list->C_Finalize(NULL);
        if (rv != CKR_OK) {
            /* Log about it, but continue cleaning up */
            s_raise_ck_error(pkcs11_lib, "C_Finalize", rv);
        }
    }

    aws_shared_library_clean_up(&pkcs11_lib->shared_lib);
    aws_mem_release(pkcs11_lib->allocator, pkcs11_lib);
}

struct aws_pkcs11_lib *aws_pkcs11_lib_new(
    struct aws_allocator *allocator,
    const struct aws_pkcs11_lib_options *options) {

    /* Validate options */
    switch (options->initialize_finalize_behavior) {
        case AWS_PKCS11_LIB_DEFAULT_BEHAVIOR:
        case AWS_PKCS11_LIB_OMIT_INITIALIZE:
        case AWS_PKCS11_LIB_STRICT_INITIALIZE_FINALIZE:
            break;
        default:
            AWS_LOGF_ERROR(AWS_LS_IO_PKCS11, "Invalid PKCS#11 behavior arg: %d", options->initialize_finalize_behavior);
            aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
            return NULL;
    }

    /* Create the struct */
    struct aws_pkcs11_lib *pkcs11_lib = aws_mem_calloc(allocator, 1, sizeof(struct aws_pkcs11_lib));
    aws_ref_count_init(&pkcs11_lib->ref_count, pkcs11_lib, s_pkcs11_lib_destroy);
    pkcs11_lib->allocator = allocator;

    /* Load the library. */

    /* need a null-terminated string to call next function,
     * or NULL if going to search the current application for PKCS#11 symbols. */
    struct aws_string *filename_storage = NULL;
    const char *filename = NULL;
    if (options->filename.ptr != NULL) {
        filename_storage = aws_string_new_from_cursor(allocator, &options->filename);
        filename = aws_string_c_str(filename_storage);
    }

    AWS_LOGF_DEBUG(
        AWS_LS_IO_PKCS11,
        "Loading PKCS#11. file:'%s' C_Initialize:%s",
        filename ? filename : "<MAIN PROGRAM>",
        (options->initialize_finalize_behavior == AWS_PKCS11_LIB_OMIT_INITIALIZE) ? "omit" : "yes");

    if (aws_shared_library_init(&pkcs11_lib->shared_lib, filename)) {
        goto error;
    }

    /* Find C_GetFunctionList() and call it to get the list of pointers to all the other functions */
    CK_C_GetFunctionList get_function_list = NULL;
    if (aws_shared_library_find_function(
            &pkcs11_lib->shared_lib, "C_GetFunctionList", (aws_generic_function *)&get_function_list)) {
        goto error;
    }

    CK_RV rv = get_function_list(&pkcs11_lib->function_list);
    if (rv != CKR_OK) {
        s_raise_ck_error(pkcs11_lib, "C_GetFunctionList", rv);
        goto error;
    }

    /* Check function list's API version */
    CK_VERSION version = pkcs11_lib->function_list->version;
    if ((version.major != AWS_SUPPORTED_CRYPTOKI_VERSION_MAJOR) ||
        (version.minor < AWS_MIN_SUPPORTED_CRYPTOKI_VERSION_MINOR)) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_PKCS11,
            "id=%p: Library implements PKCS#11 version %" PRIu8 ".%" PRIu8 " but %d.%d compatibility is required",
            (void *)pkcs11_lib,
            version.major,
            version.minor,
            AWS_SUPPORTED_CRYPTOKI_VERSION_MAJOR,
            AWS_MIN_SUPPORTED_CRYPTOKI_VERSION_MINOR);

        aws_raise_error(AWS_ERROR_PKCS11_VERSION_UNSUPPORTED);
        goto error;
    }

    /* Call C_Initialize() */
    const char *init_logging_str = "omit";
    if (options->initialize_finalize_behavior != AWS_PKCS11_LIB_OMIT_INITIALIZE) {
        CK_C_INITIALIZE_ARGS init_args = {
            /* encourage lib to use our locks */
            .CreateMutex = s_pkcs11_create_mutex,
            .DestroyMutex = s_pkcs11_destroy_mutex,
            .LockMutex = s_pkcs11_lock_mutex,
            .UnlockMutex = s_pkcs11_unlock_mutex,
            /* but if it needs to use OS locks instead, sure whatever you do you */
            .flags = CKF_OS_LOCKING_OK,
        };

        rv = pkcs11_lib->function_list->C_Initialize(&init_args);
        if (rv != CKR_OK) {
            /* Ignore already-initialized errors (unless user wants STRICT behavior) */
            if (rv != CKR_CRYPTOKI_ALREADY_INITIALIZED ||
                options->initialize_finalize_behavior == AWS_PKCS11_LIB_STRICT_INITIALIZE_FINALIZE) {

                s_raise_ck_error(pkcs11_lib, "C_Initialize", rv);
                goto error;
            }
        }

        init_logging_str = aws_pkcs11_ckr_str(rv);

        if (options->initialize_finalize_behavior == AWS_PKCS11_LIB_STRICT_INITIALIZE_FINALIZE) {
            pkcs11_lib->finalize_on_cleanup = true;
        }
    }

    /* Get info about the library and log it.
     * This will be VERY useful for diagnosing user issues. */
    CK_INFO info;
    AWS_ZERO_STRUCT(info);
    rv = pkcs11_lib->function_list->C_GetInfo(&info);
    if (rv != CKR_OK) {
        s_raise_ck_error(pkcs11_lib, "C_GetInfo", rv);
        goto error;
    }

    AWS_LOGF_INFO(
        AWS_LS_IO_PKCS11,
        "id=%p: PKCS#11 loaded. file:'%s' cryptokiVersion:%" PRIu8 ".%" PRIu8 " manufacturerID:'" PRInSTR
        "' flags:0x%08lX libraryDescription:'" PRInSTR "' libraryVersion:%" PRIu8 ".%" PRIu8 " C_Initialize:%s",
        (void *)pkcs11_lib,
        filename ? filename : "<MAIN PROGRAM>",
        info.cryptokiVersion.major,
        info.cryptokiVersion.minor,
        AWS_BYTE_CURSOR_PRI(s_trim_padding(info.manufacturerID, sizeof(info.manufacturerID))),
        info.flags,
        AWS_BYTE_CURSOR_PRI(s_trim_padding(info.libraryDescription, sizeof(info.libraryDescription))),
        info.libraryVersion.major,
        info.libraryVersion.minor,
        init_logging_str);

    /* Success! */
    goto clean_up;

error:
    AWS_LOGF_ERROR(
        AWS_LS_IO_PKCS11,
        "id=%p: Failed to initialize PKCS#11 library from '%s'",
        (void *)pkcs11_lib,
        filename ? filename : "<MAIN_PROGRAM>");

    aws_pkcs11_lib_release(pkcs11_lib);
    pkcs11_lib = NULL;

clean_up:
    aws_string_destroy(filename_storage);
    return pkcs11_lib;
}

struct aws_pkcs11_lib *aws_pkcs11_lib_acquire(struct aws_pkcs11_lib *pkcs11_lib) {
    aws_ref_count_acquire(&pkcs11_lib->ref_count);
    return pkcs11_lib;
}

void aws_pkcs11_lib_release(struct aws_pkcs11_lib *pkcs11_lib) {
    if (pkcs11_lib) {
        aws_ref_count_release(&pkcs11_lib->ref_count);
    }
}

/**
 * Find the slot that meets all criteria:
 * - has a token
 * - if match_slot_id is non-null, then slot IDs must match
 * - if match_token_label is non-null, then labels must match
 * The function fails unless it finds exactly one slot meeting all criteria.
 */
int aws_pkcs11_lib_find_slot_with_token(
    struct aws_pkcs11_lib *pkcs11_lib,
    const uint64_t *match_slot_id,
    const struct aws_string *match_token_label,
    CK_SLOT_ID *out_slot_id) {

    CK_SLOT_ID *slot_id_array = NULL; /* array of IDs */
    CK_SLOT_ID *candidate = NULL;     /* points to ID in slot_id_array */
    CK_TOKEN_INFO info;
    AWS_ZERO_STRUCT(info);
    bool success = false;

    /* query number of slots with tokens */
    CK_ULONG num_slots = 0;
    CK_RV rv = pkcs11_lib->function_list->C_GetSlotList(CK_TRUE /*tokenPresent*/, NULL /*pSlotList*/, &num_slots);
    if (rv != CKR_OK) {
        s_raise_ck_error(pkcs11_lib, "C_GetSlotList", rv);
        goto clean_up;
    }

    if (num_slots == 0) {
        AWS_LOGF_ERROR(AWS_LS_IO_PKCS11, "id=%p: No PKCS#11 tokens present in any slot", (void *)pkcs11_lib);
        aws_raise_error(AWS_ERROR_PKCS11_TOKEN_NOT_FOUND);
        goto clean_up;
    }

    AWS_LOGF_TRACE(
        AWS_LS_IO_PKCS11, "id=%p: Found %lu slots with tokens. Picking one...", (void *)pkcs11_lib, num_slots);

    /* allocate space for slot IDs */
    slot_id_array = aws_mem_calloc(pkcs11_lib->allocator, num_slots, sizeof(CK_SLOT_ID));

    /* query all slot IDs */
    rv = pkcs11_lib->function_list->C_GetSlotList(CK_TRUE /*tokenPresent*/, slot_id_array, &num_slots);
    if (rv != CKR_OK) {
        s_raise_ck_error(pkcs11_lib, "C_GetSlotList", rv);
        goto clean_up;
    }

    for (size_t i = 0; i < num_slots; ++i) {
        CK_SLOT_ID slot_id_i = slot_id_array[i];

        /* if specific slot_id requested, and this isn't it, then skip */
        if ((match_slot_id != NULL) && (*match_slot_id != slot_id_i)) {
            AWS_LOGF_TRACE(
                AWS_LS_IO_PKCS11,
                "id=%p: Ignoring PKCS#11 token because slot %lu doesn't match %" PRIu64,
                (void *)pkcs11_lib,
                slot_id_i,
                *match_slot_id);
            continue;
        }

        /* query token info */
        CK_TOKEN_INFO token_info_i;
        AWS_ZERO_STRUCT(token_info_i);
        rv = pkcs11_lib->function_list->C_GetTokenInfo(slot_id_i, &token_info_i);
        if (rv != CKR_OK) {
            s_raise_ck_error(pkcs11_lib, "C_GetTokenInfo", rv);
            goto clean_up;
        }

        /* if specific token label requested, and this isn't it, then skip */
        if (match_token_label != NULL) {
            struct aws_byte_cursor label_i = s_trim_padding(token_info_i.label, sizeof(token_info_i.label));
            if (aws_string_eq_byte_cursor(match_token_label, &label_i) == false) {
                AWS_LOGF_TRACE(
                    AWS_LS_IO_PKCS11,
                    "id=%p: Ignoring PKCS#11 token in slot %lu because label '" PRInSTR "' doesn't match '%s'",
                    (void *)pkcs11_lib,
                    slot_id_i,
                    AWS_BYTE_CURSOR_PRI(label_i),
                    aws_string_c_str(match_token_label));
                continue;
            }
        }

        /* this slot is a candidate! */

        /* be sure there's only one candidate */
        if (candidate != NULL) {
            AWS_LOGF_ERROR(
                AWS_LS_IO_PKCS11,
                "id=%p: Failed to choose PKCS#11 token, multiple tokens match search criteria",
                (void *)pkcs11_lib);
            aws_raise_error(AWS_ERROR_PKCS11_TOKEN_NOT_FOUND);
            goto clean_up;
        }

        /* the new candidate! */
        candidate = &slot_id_array[i];
        memcpy(&info, &token_info_i, sizeof(CK_TOKEN_INFO));
    }

    if (candidate == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_PKCS11, "id=%p: Failed to find PKCS#11 token which matches search criteria", (void *)pkcs11_lib);
        aws_raise_error(AWS_ERROR_PKCS11_TOKEN_NOT_FOUND);
        goto clean_up;
    }

    /* success! */
    AWS_LOGF_DEBUG(
        AWS_LS_IO_PKCS11,
        "id=%p: Selected PKCS#11 token. slot:%lu label:'" PRInSTR "' manufacturerID:'" PRInSTR "' model:'" PRInSTR
        "' serialNumber:'" PRInSTR "' flags:0x%08lX sessionCount:%lu/%lu rwSessionCount:%lu/%lu"
        " freePublicMemory:%lu/%lu freePrivateMemory:%lu/%lu"
        " hardwareVersion:%" PRIu8 ".%" PRIu8 " firmwareVersion:%" PRIu8 ".%" PRIu8,
        (void *)pkcs11_lib,
        *candidate,
        AWS_BYTE_CURSOR_PRI(s_trim_padding(info.label, sizeof(info.label))),
        AWS_BYTE_CURSOR_PRI(s_trim_padding(info.manufacturerID, sizeof(info.manufacturerID))),
        AWS_BYTE_CURSOR_PRI(s_trim_padding(info.model, sizeof(info.model))),
        AWS_BYTE_CURSOR_PRI(s_trim_padding(info.serialNumber, sizeof(info.serialNumber))),
        info.flags,
        info.ulSessionCount,
        info.ulMaxSessionCount,
        info.ulRwSessionCount,
        info.ulMaxRwSessionCount,
        info.ulFreePublicMemory,
        info.ulTotalPublicMemory,
        info.ulFreePrivateMemory,
        info.ulTotalPrivateMemory,
        info.hardwareVersion.major,
        info.hardwareVersion.minor,
        info.firmwareVersion.major,
        info.firmwareVersion.minor);

    *out_slot_id = *candidate;
    success = true;

clean_up:
    aws_mem_release(pkcs11_lib->allocator, slot_id_array);
    return success ? AWS_OP_SUCCESS : AWS_OP_ERR;
}

CK_FUNCTION_LIST *aws_pkcs11_lib_get_function_list(struct aws_pkcs11_lib *pkcs11_lib) {
    return pkcs11_lib->function_list;
}

int aws_pkcs11_lib_open_session(
    struct aws_pkcs11_lib *pkcs11_lib,
    CK_SLOT_ID slot_id,
    CK_SESSION_HANDLE *out_session_handle) {

    CK_SESSION_HANDLE session_handle = CK_INVALID_HANDLE;
    CK_RV rv = pkcs11_lib->function_list->C_OpenSession(
        slot_id, CKF_SERIAL_SESSION /*flags*/, NULL /*pApplication*/, NULL /*notify*/, &session_handle);
    if (rv != CKR_OK) {
        return s_raise_ck_error(pkcs11_lib, "C_OpenSession", rv);
    }

    /* success! */
    AWS_LOGF_DEBUG(
        AWS_LS_IO_PKCS11, "id=%p session=%lu: Session opened on slot %lu", (void *)pkcs11_lib, session_handle, slot_id);

    *out_session_handle = session_handle;
    return AWS_OP_SUCCESS;
}

void aws_pkcs11_lib_close_session(struct aws_pkcs11_lib *pkcs11_lib, CK_SESSION_HANDLE session_handle) {
    CK_RV rv = pkcs11_lib->function_list->C_CloseSession(session_handle);
    if (rv == CKR_OK) {
        AWS_LOGF_DEBUG(AWS_LS_IO_PKCS11, "id=%p session=%lu: Session closed", (void *)pkcs11_lib, session_handle);
    } else {
        /* Log the error, but we can't really do anything about it */
        AWS_LOGF_WARN(
            AWS_LS_IO_PKCS11,
            "id=%p session=%lu: Ignoring C_CloseSession() failure. PKCS#11 error: %s (0x%08lX)",
            (void *)pkcs11_lib,
            session_handle,
            aws_pkcs11_ckr_str(rv),
            rv);
    }
}

int aws_pkcs11_lib_login_user(
    struct aws_pkcs11_lib *pkcs11_lib,
    CK_SESSION_HANDLE session_handle,
    const struct aws_string *optional_user_pin) {

    CK_UTF8CHAR_PTR pin = NULL;
    CK_ULONG pin_len = 0;
    if (optional_user_pin) {
        if (optional_user_pin->len > ULONG_MAX) {
            AWS_LOGF_ERROR(AWS_LS_IO_PKCS11, "id=%p session=%lu: PIN is too long", (void *)pkcs11_lib, session_handle);
            return aws_raise_error(AWS_ERROR_PKCS11_CKR_PIN_INCORRECT);
        }
        pin_len = (CK_ULONG)optional_user_pin->len;
        pin = (CK_UTF8CHAR_PTR)optional_user_pin->bytes;
    }

    CK_RV rv = pkcs11_lib->function_list->C_Login(session_handle, CKU_USER, pin, pin_len);
    /* Ignore if we are already logged in, this could happen if application using device sdk also logs in to pkcs11 */
    if (rv != CKR_OK && rv != CKR_USER_ALREADY_LOGGED_IN) {
        return s_raise_ck_session_error(pkcs11_lib, "C_Login", session_handle, rv);
    }

    /* Success! */
    if (rv == CKR_USER_ALREADY_LOGGED_IN) {
        AWS_LOGF_DEBUG(
            AWS_LS_IO_PKCS11, "id=%p session=%lu: User was already logged in", (void *)pkcs11_lib, session_handle);
    } else {
        AWS_LOGF_DEBUG(AWS_LS_IO_PKCS11, "id=%p session=%lu: User logged in", (void *)pkcs11_lib, session_handle);
    }
    return AWS_OP_SUCCESS;
}

/**
 * Find the object that meets all criteria:
 * - is private key
 * - if match_label is non-null, then labels must match
 * The function fails unless it finds exactly one object meeting all criteria.
 */
int aws_pkcs11_lib_find_private_key(
    struct aws_pkcs11_lib *pkcs11_lib,
    CK_SESSION_HANDLE session_handle,
    const struct aws_string *match_label,
    CK_OBJECT_HANDLE *out_key_handle,
    CK_KEY_TYPE *out_key_type) {

    /* gets set true after everything succeeds */
    bool success = false;

    /* gets set true after search initialized.
     * indicates that C_FindObjectsFinal() must be run before function ends */
    bool must_finalize_search = false;

    /* set up search attributes */
    CK_OBJECT_CLASS key_class = CKO_PRIVATE_KEY;
    CK_ULONG num_attributes = 1;
    CK_ATTRIBUTE attributes[2] = {
        {
            .type = CKA_CLASS,
            .pValue = &key_class,
            .ulValueLen = sizeof(key_class),
        },
    };

    if (match_label != NULL) {
        if (match_label->len > ULONG_MAX) {
            AWS_LOGF_ERROR(
                AWS_LS_IO_PKCS11,
                "id=%p session=%lu: private key label is too long",
                (void *)pkcs11_lib,
                session_handle);
            aws_raise_error(AWS_ERROR_PKCS11_KEY_NOT_FOUND);
            goto clean_up;
        }

        CK_ATTRIBUTE *attr = &attributes[num_attributes++];
        attr->type = CKA_LABEL;
        attr->pValue = (void *)match_label->bytes;
        attr->ulValueLen = (CK_ULONG)match_label->len;
    }

    /* initialize search */
    CK_RV rv = pkcs11_lib->function_list->C_FindObjectsInit(session_handle, attributes, num_attributes);
    if (rv != CKR_OK) {
        s_raise_ck_session_error(pkcs11_lib, "C_FindObjectsInit", session_handle, rv);
        goto clean_up;
    }

    must_finalize_search = true;

    /* get search results.
     * note that we're asking for 2 objects max, so we can fail if we find more than one */
    CK_OBJECT_HANDLE found_objects[2] = {0};
    CK_ULONG num_found = 0;
    rv = pkcs11_lib->function_list->C_FindObjects(session_handle, found_objects, 2 /*max*/, &num_found);
    if (rv != CKR_OK) {
        s_raise_ck_session_error(pkcs11_lib, "C_FindObjects", session_handle, rv);
        goto clean_up;
    }

    if ((num_found == 0) || (found_objects[0] == CK_INVALID_HANDLE)) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_PKCS11,
            "id=%p session=%lu: Failed to find private key on PKCS#11 token which matches search criteria",
            (void *)pkcs11_lib,
            session_handle);
        aws_raise_error(AWS_ERROR_PKCS11_KEY_NOT_FOUND);
        goto clean_up;
    }
    if (num_found > 1) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_PKCS11,
            "id=%p session=%lu: Failed to choose private key, multiple objects on PKCS#11 token match search criteria",
            (void *)pkcs11_lib,
            session_handle);
        aws_raise_error(AWS_ERROR_PKCS11_KEY_NOT_FOUND);
        goto clean_up;
    }

    /* key found */
    CK_OBJECT_HANDLE key_handle = found_objects[0];

    /* query key-type */
    CK_KEY_TYPE key_type = 0;
    CK_ATTRIBUTE key_attributes[] = {
        {
            .type = CKA_KEY_TYPE,
            .pValue = &key_type,
            .ulValueLen = sizeof(key_type),
        },
    };

    rv = pkcs11_lib->function_list->C_GetAttributeValue(
        session_handle, key_handle, key_attributes, AWS_ARRAY_SIZE(key_attributes));
    if (rv != CKR_OK) {
        s_raise_ck_session_error(pkcs11_lib, "C_GetAttributeValue", session_handle, rv);
        goto clean_up;
    }

    switch (key_type) {
        case CKK_RSA:
        case CKK_EC:
            break;
        default:
            AWS_LOGF_ERROR(
                AWS_LS_IO_PKCS11,
                "id=%p session=%lu: PKCS#11 private key type %s (0x%08lX) is currently unsupported",
                (void *)pkcs11_lib,
                session_handle,
                s_ckk_str(key_type),
                key_type);
            aws_raise_error(AWS_ERROR_PKCS11_KEY_TYPE_UNSUPPORTED);
            goto clean_up;
    }

    /* Success! */
    AWS_LOGF_TRACE(
        AWS_LS_IO_PKCS11,
        "id=%p session=%lu: Found private key. type=%s",
        (void *)pkcs11_lib,
        session_handle,
        s_ckk_str(key_type));
    *out_key_handle = key_handle;
    *out_key_type = key_type;
    success = true;

clean_up:

    if (must_finalize_search) {
        rv = pkcs11_lib->function_list->C_FindObjectsFinal(session_handle);
        /* don't bother reporting error if we were already failing */
        if ((rv != CKR_OK) && (success == true)) {
            s_raise_ck_session_error(pkcs11_lib, "C_FindObjectsFinal", session_handle, rv);
            success = false;
        }
    }

    return success ? AWS_OP_SUCCESS : AWS_OP_ERR;
}

int aws_pkcs11_lib_decrypt(
    struct aws_pkcs11_lib *pkcs11_lib,
    CK_SESSION_HANDLE session_handle,
    CK_OBJECT_HANDLE key_handle,
    CK_KEY_TYPE key_type,
    struct aws_byte_cursor encrypted_data,
    struct aws_allocator *allocator,
    struct aws_byte_buf *out_data) {

    AWS_ASSERT(encrypted_data.len <= ULONG_MAX); /* do real error checking if this becomes a public API */
    AWS_ASSERT(out_data->allocator == NULL);

    CK_MECHANISM mechanism;
    AWS_ZERO_STRUCT(mechanism);

    /* Note, CKK_EC is not expected to enter into this code path */
    switch (key_type) {
        case CKK_RSA:
            mechanism.mechanism = CKM_RSA_PKCS;
            break;
        default:
            aws_raise_error(AWS_ERROR_PKCS11_KEY_TYPE_UNSUPPORTED);
            goto error;
    }

    /* initialize the decryption operation */
    CK_RV rv = pkcs11_lib->function_list->C_DecryptInit(session_handle, &mechanism, key_handle);
    if (rv != CKR_OK) {
        s_raise_ck_session_error(pkcs11_lib, "C_DecryptInit", session_handle, rv);
        goto error;
    }

    /* query needed capacity (finalizes decryption operation if it fails) */
    CK_ULONG data_len = 0;
    rv = pkcs11_lib->function_list->C_Decrypt(
        session_handle, encrypted_data.ptr, (CK_ULONG)encrypted_data.len, NULL /*pData*/, &data_len);
    if (rv != CKR_OK) {
        s_raise_ck_session_error(pkcs11_lib, "C_Decrypt", session_handle, rv);
        goto error;
    }

    aws_byte_buf_init(out_data, allocator, data_len); /* cannot fail */

    /* do actual decrypt (finalizes decryption operation, whether it succeeds or fails)*/
    rv = pkcs11_lib->function_list->C_Decrypt(
        session_handle, encrypted_data.ptr, (CK_ULONG)encrypted_data.len, out_data->buffer, &data_len);
    if (rv != CKR_OK) {
        s_raise_ck_session_error(pkcs11_lib, "C_Decrypt", session_handle, rv);
        goto error;
    }

    out_data->len = data_len;
    return AWS_OP_SUCCESS;

error:
    aws_byte_buf_clean_up(out_data);
    return AWS_OP_ERR;
}

/* runs C_Sign(), putting encrypted message into out_signature */
static int s_pkcs11_sign_helper(
    struct aws_pkcs11_lib *pkcs11_lib,
    CK_SESSION_HANDLE session_handle,
    CK_OBJECT_HANDLE key_handle,
    CK_MECHANISM mechanism,
    struct aws_byte_cursor input_data,
    struct aws_allocator *allocator,
    struct aws_byte_buf *out_signature) {

    /* initialize signing operation */
    CK_RV rv = pkcs11_lib->function_list->C_SignInit(session_handle, &mechanism, key_handle);
    if (rv != CKR_OK) {
        s_raise_ck_session_error(pkcs11_lib, "C_SignInit", session_handle, rv);
        goto error;
    }

    /* query needed capacity (finalizes signing operation if it fails) */
    CK_ULONG signature_len = 0;
    rv = pkcs11_lib->function_list->C_Sign(
        session_handle, input_data.ptr, (CK_ULONG)input_data.len, NULL /*pSignature*/, &signature_len);
    if (rv != CKR_OK) {
        s_raise_ck_session_error(pkcs11_lib, "C_Sign", session_handle, rv);
        goto error;
    }

    aws_byte_buf_init(out_signature, allocator, signature_len); /* cannot fail */

    /* do actual signing (finalizes signing operation, whether it succeeds or fails) */
    rv = pkcs11_lib->function_list->C_Sign(
        session_handle, input_data.ptr, (CK_ULONG)input_data.len, out_signature->buffer, &signature_len);
    if (rv != CKR_OK) {
        s_raise_ck_session_error(pkcs11_lib, "C_Sign", session_handle, rv);
        goto error;
    }

    out_signature->len = signature_len;
    return AWS_OP_SUCCESS;

error:
    aws_byte_buf_clean_up(out_signature);
    return AWS_OP_ERR;
}

int aws_get_prefix_to_rsa_sig(enum aws_tls_hash_algorithm digest_alg, struct aws_byte_cursor *out_prefix) {
    switch (digest_alg) {
        case AWS_TLS_HASH_SHA1:
            *out_prefix = aws_byte_cursor_from_array(SHA1_PREFIX_TO_RSA_SIG, sizeof(SHA1_PREFIX_TO_RSA_SIG));
            break;
        case AWS_TLS_HASH_SHA224:
            *out_prefix = aws_byte_cursor_from_array(SHA224_PREFIX_TO_RSA_SIG, sizeof(SHA224_PREFIX_TO_RSA_SIG));
            break;
        case AWS_TLS_HASH_SHA256:
            *out_prefix = aws_byte_cursor_from_array(SHA256_PREFIX_TO_RSA_SIG, sizeof(SHA256_PREFIX_TO_RSA_SIG));
            break;
        case AWS_TLS_HASH_SHA384:
            *out_prefix = aws_byte_cursor_from_array(SHA384_PREFIX_TO_RSA_SIG, sizeof(SHA384_PREFIX_TO_RSA_SIG));
            break;
        case AWS_TLS_HASH_SHA512:
            *out_prefix = aws_byte_cursor_from_array(SHA512_PREFIX_TO_RSA_SIG, sizeof(SHA512_PREFIX_TO_RSA_SIG));
            break;
        default:
            return aws_raise_error(AWS_IO_TLS_DIGEST_ALGORITHM_UNSUPPORTED);
    }
    return AWS_OP_SUCCESS;
}

static int s_pkcs11_sign_rsa(
    struct aws_pkcs11_lib *pkcs11_lib,
    CK_SESSION_HANDLE session_handle,
    CK_OBJECT_HANDLE key_handle,
    struct aws_byte_cursor digest_data,
    struct aws_allocator *allocator,
    enum aws_tls_hash_algorithm digest_alg,
    enum aws_tls_signature_algorithm signature_alg,
    struct aws_byte_buf *out_signature) {

    if (signature_alg != AWS_TLS_SIGNATURE_RSA) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_PKCS11,
            "id=%p session=%lu: Signature algorithm '%s' is currently unsupported for PKCS#11 RSA keys. "
            "Supported algorithms are: RSA",
            (void *)pkcs11_lib,
            session_handle,
            aws_tls_signature_algorithm_str(signature_alg));
        return aws_raise_error(AWS_IO_TLS_SIGNATURE_ALGORITHM_UNSUPPORTED);
    }

    struct aws_byte_cursor prefix;
    if (aws_get_prefix_to_rsa_sig(digest_alg, &prefix)) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_PKCS11,
            "id=%p session=%lu: Unsupported digest '%s' for PKCS#11 RSA signing. "
            "Supported digests are: SHA1, SHA256, SHA384 and SHA512. AWS error: %s",
            (void *)pkcs11_lib,
            session_handle,
            aws_tls_hash_algorithm_str(digest_alg),
            aws_error_name(aws_last_error()));
        return AWS_OP_ERR;
    }

    bool success = false;

    struct aws_byte_buf prefixed_input;
    aws_byte_buf_init(&prefixed_input, allocator, digest_data.len + prefix.len); /* cannot fail */
    aws_byte_buf_write_from_whole_cursor(&prefixed_input, prefix);
    aws_byte_buf_write_from_whole_cursor(&prefixed_input, digest_data);

    /* We could get the original input and not the digest to sign and leverage CKM_SHA*_RSA_PKCS mechanisms
     * but the original input is too large (all the TLS handshake messages until clientCertVerify) and
     * we do not want to perform the digest inside the TPM for performance reasons, therefore we only
     * leverage CKM_RSA_PKCS mechanism and *only* sign the digest using TPM. Only signing requires
     * additional prefix to the input to complete the digest part for RSA signing. */
    CK_MECHANISM mechanism = {.mechanism = CKM_RSA_PKCS};

    if (s_pkcs11_sign_helper(
            pkcs11_lib,
            session_handle,
            key_handle,
            mechanism,
            aws_byte_cursor_from_buf(&prefixed_input),
            allocator,
            out_signature)) {
        goto error;
    }

    success = true;
    goto clean_up;

error:
    aws_byte_buf_clean_up(out_signature);
clean_up:
    aws_byte_buf_clean_up(&prefixed_input);
    return success ? AWS_OP_SUCCESS : AWS_OP_ERR;
}

/*
 * Basic ASN.1 (DER) encoding of header -- sufficient for ECDSA
 */
static int s_asn1_enc_prefix(struct aws_byte_buf *buffer, uint8_t identifier, size_t length) {
    if (((identifier & 0x1f) == 0x1f) || (length > 0x7f)) {
        AWS_LOGF_ERROR(AWS_LS_IO_PKCS11, "Unable to encode ASN.1 (DER) header 0x%02x %zu", identifier, length);
        return aws_raise_error(AWS_ERROR_PKCS11_ENCODING_ERROR);
    }
    uint8_t head[2];
    head[0] = identifier;
    head[1] = (uint8_t)length;
    if (!aws_byte_buf_write(buffer, head, sizeof(head))) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_PKCS11, "Insufficient buffer to encode ASN.1 (DER) header 0x%02x %zu", identifier, length);
        return aws_raise_error(AWS_ERROR_PKCS11_ENCODING_ERROR);
    }
    return AWS_OP_SUCCESS;
}

/*
 * Basic ASN.1 (DER) encoding of an unsigned big number -- sufficient for ECDSA. Note that this implementation
 * may reduce the number of integer bytes down to 1 (removing leading zero bytes), or conversely increase by
 * one extra byte to ensure the unsigned integer is unambiguously encoded.
 */
int aws_pkcs11_asn1_enc_ubigint(struct aws_byte_buf *const buffer, struct aws_byte_cursor bigint) {

    // trim out all leading zero's
    while (bigint.len > 0 && bigint.ptr[0] == 0) {
        aws_byte_cursor_advance(&bigint, 1);
    }

    // If the most significant bit is a '1', prefix with a zero-byte to prevent misinterpreting number as negative.
    // If the big integer value was zero, length will be zero, replace with zero-byte using the same approach.
    bool add_leading_zero = bigint.len == 0 || (bigint.ptr[0] & 0x80) != 0;
    size_t actual_len = bigint.len + (add_leading_zero ? 1 : 0);

    // header - indicate integer of given length (including any prefix zero)
    bool success = s_asn1_enc_prefix(buffer, 0x02, actual_len) == AWS_OP_SUCCESS;
    if (add_leading_zero) {
        success = success && aws_byte_buf_write_u8(buffer, 0);
    }
    // write rest of number
    success = success && aws_byte_buf_write_from_whole_cursor(buffer, bigint);
    if (success) {
        return AWS_OP_SUCCESS;
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_IO_PKCS11, "Insufficient buffer to ASN.1 (DER) encode big integer of length %zu", actual_len);
        return aws_raise_error(AWS_ERROR_PKCS11_ENCODING_ERROR);
    }
}

static int s_pkcs11_sign_ecdsa(
    struct aws_pkcs11_lib *pkcs11_lib,
    CK_SESSION_HANDLE session_handle,
    CK_OBJECT_HANDLE key_handle,
    struct aws_byte_cursor digest_data,
    struct aws_allocator *allocator,
    enum aws_tls_signature_algorithm signature_alg,
    struct aws_byte_buf *out_signature) {

    struct aws_byte_buf part_signature;
    struct aws_byte_buf r_part;
    struct aws_byte_buf s_part;
    AWS_ZERO_STRUCT(part_signature);
    AWS_ZERO_STRUCT(r_part);
    AWS_ZERO_STRUCT(s_part);

    if (signature_alg != AWS_TLS_SIGNATURE_ECDSA) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_PKCS11,
            "id=%p session=%lu: Signature algorithm '%s' is currently unsupported for PKCS#11 EC keys. "
            "Supported algorithms are: ECDSA",
            (void *)pkcs11_lib,
            session_handle,
            aws_tls_signature_algorithm_str(signature_alg));
        return aws_raise_error(AWS_IO_TLS_SIGNATURE_ALGORITHM_UNSUPPORTED);
    }

    bool success = false;

    /* ECDSA signing consists of DER-encoding of "r" and "s" parameters. C_Sign returns the two
     * integers as big numbers in big-endian format, so translation is required.
     */
    CK_MECHANISM mechanism = {.mechanism = CKM_ECDSA};

    if (s_pkcs11_sign_helper(
            pkcs11_lib, session_handle, key_handle, mechanism, digest_data, allocator, &part_signature) !=
        AWS_OP_SUCCESS) {
        goto error;
    }

    /* PKCS11 library returns these parameters as two big unsigned integer numbers of exactly the same length. The
     * numbers need to be ASN.1/DER encoded (variable length). In addition to the header, space is needed to allow for
     * an occasional extra 0x00 prefix byte to ensure integer is encoded and interpreted as unsigned.
     */
    if (part_signature.len == 0 || (part_signature.len & 1) != 0) {
        /* This should never happen, we would fail anyway, but making it explicit and fail early */
        AWS_LOGF_ERROR(
            AWS_LS_IO_PKCS11,
            "PKCS11 library returned an invalid length, unable to interpret ECDSA signature to encode correctly.");
        return aws_raise_error(AWS_ERROR_PKCS11_ENCODING_ERROR);
        goto error;
    }
    size_t num_bytes = part_signature.len / 2;
    aws_byte_buf_init(&r_part, allocator, num_bytes + 4);
    aws_byte_buf_init(&s_part, allocator, num_bytes + 4);

    if (aws_pkcs11_asn1_enc_ubigint(&r_part, aws_byte_cursor_from_array(part_signature.buffer, num_bytes)) !=
        AWS_OP_SUCCESS) {
        goto error;
    }
    if (aws_pkcs11_asn1_enc_ubigint(
            &s_part, aws_byte_cursor_from_array(part_signature.buffer + num_bytes, num_bytes)) != AWS_OP_SUCCESS) {
        goto error;
    }
    size_t pair_len = r_part.len + s_part.len;
    aws_byte_buf_init(out_signature, allocator, pair_len + 2); // inc header
    if (s_asn1_enc_prefix(out_signature, 0x30, pair_len) != AWS_OP_SUCCESS) {
        goto error;
    }
    if (!aws_byte_buf_write_from_whole_buffer(out_signature, r_part)) {
        AWS_LOGF_ERROR(AWS_LS_IO_PKCS11, "Insufficient buffer to ASN.1 (DER) encode ECDSA signature R-part.");
        return aws_raise_error(AWS_ERROR_PKCS11_ENCODING_ERROR);
        goto error;
    }
    if (!aws_byte_buf_write_from_whole_buffer(out_signature, s_part)) {
        AWS_LOGF_ERROR(AWS_LS_IO_PKCS11, "Insufficient buffer to ASN.1 (DER) encode ECDSA signature S-part.");
        return aws_raise_error(AWS_ERROR_PKCS11_ENCODING_ERROR);
        goto error;
    }
    success = true;
    goto clean_up;

error:
    aws_byte_buf_clean_up(out_signature);
clean_up:
    aws_byte_buf_clean_up(&part_signature);
    aws_byte_buf_clean_up(&r_part);
    aws_byte_buf_clean_up(&s_part);
    return success ? AWS_OP_SUCCESS : AWS_OP_ERR;
}

int aws_pkcs11_lib_sign(
    struct aws_pkcs11_lib *pkcs11_lib,
    CK_SESSION_HANDLE session_handle,
    CK_OBJECT_HANDLE key_handle,
    CK_KEY_TYPE key_type,
    struct aws_byte_cursor digest_data,
    struct aws_allocator *allocator,
    enum aws_tls_hash_algorithm digest_alg,
    enum aws_tls_signature_algorithm signature_alg,
    struct aws_byte_buf *out_signature) {

    AWS_ASSERT(digest_data.len <= ULONG_MAX); /* do real error checking if this becomes a public API */
    AWS_ASSERT(out_signature->allocator == NULL);

    switch (key_type) {
        case CKK_RSA:
            return s_pkcs11_sign_rsa(
                pkcs11_lib,
                session_handle,
                key_handle,
                digest_data,
                allocator,
                digest_alg,
                signature_alg,
                out_signature);
        case CKK_EC:
            return s_pkcs11_sign_ecdsa(
                pkcs11_lib,
                session_handle,
                key_handle,
                digest_data,
                allocator,
                // not digest_alg -- need to check this
                signature_alg,
                out_signature);
        default:
            return aws_raise_error(AWS_ERROR_PKCS11_KEY_TYPE_UNSUPPORTED);
    }
}
