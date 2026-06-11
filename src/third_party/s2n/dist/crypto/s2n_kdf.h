/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#pragma once

#if S2N_OPENSSL_VERSION_AT_LEAST(3, 0, 0)

    #include <openssl/core_names.h>
    #include <openssl/kdf.h>

    #define S2N_OSSL_PARAM_BLOB(id, blob) \
        OSSL_PARAM_octet_string(id, blob->data, blob->size)
    #define S2N_OSSL_PARAM_STR(id, cstr) \
        OSSL_PARAM_utf8_string(id, cstr, strlen(cstr))
    #define S2N_OSSL_PARAM_INT(id, val) \
        OSSL_PARAM_int(id, &val)

DEFINE_POINTER_CLEANUP_FUNC(EVP_KDF_CTX *, EVP_KDF_CTX_free);
DEFINE_POINTER_CLEANUP_FUNC(EVP_KDF *, EVP_KDF_free);

#endif /* Openssl3 only */
