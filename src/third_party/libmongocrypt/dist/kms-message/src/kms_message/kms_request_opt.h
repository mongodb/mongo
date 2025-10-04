/*
 * Copyright 2018-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef KMS_REQUEST_OPT_H
#define KMS_REQUEST_OPT_H

#include "kms_message_defines.h"

#include <stdbool.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _kms_request_opt_t kms_request_opt_t;

typedef size_t kms_request_provider_t;

#define KMS_REQUEST_PROVIDER_AWS 0
#define KMS_REQUEST_PROVIDER_AZURE 1
#define KMS_REQUEST_PROVIDER_GCP 2
#define KMS_REQUEST_PROVIDER_KMIP 3

KMS_MSG_EXPORT (kms_request_opt_t *)
kms_request_opt_new (void);

/* The default provider is AWS. This will automatically set extra headers.
 * Returns false if provider is invalid. */
KMS_MSG_EXPORT (bool)
kms_request_opt_set_provider (kms_request_opt_t *opt,
                              kms_request_provider_t provider);
KMS_MSG_EXPORT (void)
kms_request_opt_destroy (kms_request_opt_t *request);
KMS_MSG_EXPORT (void)
kms_request_opt_set_connection_close (kms_request_opt_t *opt,
                                      bool connection_close);

KMS_MSG_EXPORT (void)
kms_request_opt_set_crypto_hooks (kms_request_opt_t *opt,
                                  bool (*sha256) (void *ctx,
                                                  const char *input,
                                                  size_t len,
                                                  unsigned char *hash_out),
                                  bool (*sha256_hmac) (void *ctx,
                                                       const char *key_input,
                                                       size_t key_len,
                                                       const char *input,
                                                       size_t len,
                                                       unsigned char *hash_out),
                                  void *ctx);

KMS_MSG_EXPORT (void)
kms_request_opt_set_crypto_hook_sign_rsaes_pkcs1_v1_5 (
   kms_request_opt_t *opt,
   bool (*sign_rsaes_pkcs1_v1_5) (void *ctx,
                                  const char *private_key,
                                  size_t private_key_len,
                                  const char *input,
                                  size_t input_len,
                                  unsigned char *signature_out),
   void *ctx);
#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KMS_REQUEST_OPT_H */
