/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/cal/cal.h>
#include <aws/common/common.h>
#include <aws/common/error.h>

#define AWS_DEFINE_ERROR_INFO_CAL(CODE, STR) [(CODE) - 0x1C00] = AWS_DEFINE_ERROR_INFO(CODE, STR, "aws-c-cal")

static struct aws_error_info s_errors[] = {
    AWS_DEFINE_ERROR_INFO_CAL(AWS_ERROR_CAL_SIGNATURE_VALIDATION_FAILED, "Verify on a cryptographic signature failed."),
    AWS_DEFINE_ERROR_INFO_CAL(
        AWS_ERROR_CAL_MISSING_REQUIRED_KEY_COMPONENT,
        "An attempt was made to perform an "
        "Asymmetric cryptographic operation with the"
        "wrong key component. For example, attempt to"
        "verify a signature with a private key or "
        "sign a message with a public key."),
    AWS_DEFINE_ERROR_INFO_CAL(
        AWS_ERROR_CAL_INVALID_KEY_LENGTH_FOR_ALGORITHM,
        "A key length was used for an algorithm that needs a different key length."),
    AWS_DEFINE_ERROR_INFO_CAL(
        AWS_ERROR_CAL_UNKNOWN_OBJECT_IDENTIFIER,
        "An ASN.1 OID was encountered that wasn't expected or understood. Most likely, an unsupported algorithm was "
        "encountered."),
    AWS_DEFINE_ERROR_INFO_CAL(
        AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED,
        "An ASN.1 DER decoding operation failed on malformed input."),
    AWS_DEFINE_ERROR_INFO_CAL(
        AWS_ERROR_CAL_MISMATCHED_DER_TYPE,
        "An invalid DER type was requested during encoding/decoding."),
    AWS_DEFINE_ERROR_INFO_CAL(
        AWS_ERROR_CAL_UNSUPPORTED_ALGORITHM,
        "The specified algorithm is unsupported on this platform."),
    AWS_DEFINE_ERROR_INFO_CAL(
        AWS_ERROR_CAL_BUFFER_TOO_LARGE_FOR_ALGORITHM,
        "The input passed to a cipher algorithm was too large for that algorithm. Consider breaking the input into "
        "smaller chunks."),
    AWS_DEFINE_ERROR_INFO_CAL(
        AWS_ERROR_CAL_INVALID_CIPHER_MATERIAL_SIZE_FOR_ALGORITHM,
        "A cipher material such as an initialization vector or tag was an incorrect size for the selected algorithm."),
    AWS_DEFINE_ERROR_INFO_CAL(
        AWS_ERROR_CAL_DER_UNSUPPORTED_NEGATIVE_INT,
        "DER decoder does support negative integers."),
    AWS_DEFINE_ERROR_INFO_CAL(AWS_ERROR_CAL_UNSUPPORTED_KEY_FORMAT, "Key format is not supported."),
    AWS_DEFINE_ERROR_INFO_CAL(
        AWS_ERROR_CAL_CRYPTO_OPERATION_FAILED,
        "Unknown error when calling underlying Crypto library.")};

static struct aws_error_info_list s_list = {
    .error_list = s_errors,
    .count = AWS_ARRAY_SIZE(s_errors),
};

static struct aws_log_subject_info s_cal_log_subject_infos[] = {
    DEFINE_LOG_SUBJECT_INFO(
        AWS_LS_CAL_GENERAL,
        "aws-c-cal",
        "Subject for Cal logging that doesn't belong to any particular category"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_CAL_ECC, "ecc", "Subject for elliptic curve cryptography specific logging."),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_CAL_HASH, "hash", "Subject for hashing specific logging."),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_CAL_HMAC, "hmac", "Subject for hmac specific logging."),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_CAL_DER, "der", "Subject for der specific logging."),
    DEFINE_LOG_SUBJECT_INFO(
        AWS_LS_CAL_LIBCRYPTO_RESOLVE,
        "libcrypto_resolve",
        "Subject for libcrypto symbol resolution logging."),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_CAL_RSA, "rsa", "Subject for rsa cryptography specific logging."),
};

static struct aws_log_subject_info_list s_cal_log_subject_list = {
    .subject_list = s_cal_log_subject_infos,
    .count = AWS_ARRAY_SIZE(s_cal_log_subject_infos),
};

#ifndef BYO_CRYPTO
extern void aws_cal_platform_init(struct aws_allocator *allocator);
extern void aws_cal_platform_clean_up(void);
extern void aws_cal_platform_thread_clean_up(void);
#endif /* BYO_CRYPTO */

static bool s_cal_library_initialized = false;

void aws_cal_library_init(struct aws_allocator *allocator) {
    if (!s_cal_library_initialized) {
        aws_common_library_init(allocator);
        aws_register_error_info(&s_list);
        aws_register_log_subject_info_list(&s_cal_log_subject_list);
#ifndef BYO_CRYPTO
        aws_cal_platform_init(allocator);
#endif /* BYO_CRYPTO */
        s_cal_library_initialized = true;
    }
}
void aws_cal_library_clean_up(void) {
    if (s_cal_library_initialized) {
        s_cal_library_initialized = false;
#ifndef BYO_CRYPTO
        aws_cal_platform_clean_up();
#endif /* BYO_CRYPTO */
        aws_unregister_log_subject_info_list(&s_cal_log_subject_list);
        aws_unregister_error_info(&s_list);
        aws_common_library_clean_up();
    }
}

void aws_cal_thread_clean_up(void) {
#ifndef BYO_CRYPTO
    aws_cal_platform_thread_clean_up();
#endif /* BYO_CRYPTO */
}
