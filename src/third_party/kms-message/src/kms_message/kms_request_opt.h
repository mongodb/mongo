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

KMS_MSG_EXPORT (kms_request_opt_t *)
kms_request_opt_new (void);
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KMS_REQUEST_OPT_H */
