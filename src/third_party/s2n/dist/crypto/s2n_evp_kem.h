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

#include "tls/s2n_kem.h"

int s2n_evp_kem_generate_keypair(IN const struct s2n_kem *kem, OUT uint8_t *public_key, OUT uint8_t *private_key);
int s2n_evp_kem_encapsulate(IN const struct s2n_kem *kem, OUT uint8_t *ciphertext, OUT uint8_t *shared_secret, IN const uint8_t *public_key);
int s2n_evp_kem_decapsulate(IN const struct s2n_kem *kem, OUT uint8_t *shared_secret, IN const uint8_t *ciphertext, IN const uint8_t *private_key);
