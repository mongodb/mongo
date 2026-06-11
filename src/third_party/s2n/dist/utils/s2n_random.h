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

#include "utils/s2n_blob.h"
#include "utils/s2n_result.h"

struct s2n_rand_device {
    const char *source;
    int fd;
    dev_t dev;
    ino_t ino;
    mode_t mode;
    dev_t rdev;
};

S2N_RESULT s2n_rand_init(void);
S2N_RESULT s2n_rand_cleanup(void);
S2N_RESULT s2n_get_public_random_data(struct s2n_blob *blob);
S2N_RESULT s2n_get_private_random_data(struct s2n_blob *blob);
S2N_RESULT s2n_public_random(int64_t max, uint64_t *output);
bool s2n_use_libcrypto_rand(void);
