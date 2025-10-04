#pragma once
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
 */

#include "utils/s2n_blob.h"
#include "utils/s2n_result.h"

/**
 * Converts a binary representation of an ip address into its canonical string
 * representation. Returns 0 on success and -1 on failure.
 */
S2N_RESULT s2n_inet_ntop(int af, const void *addr, struct s2n_blob *dst);
