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

#include <s2n.h>

/**
 * Cleans up any internal thread-local resources used by s2n-tls. This function
 * is called by `s2n_cleanup`, but depending on your thread management model, 
 * it may be called directly instead. 
 * 
 * See [Initialization](https://github.com/aws/s2n-tls/blob/main/docs/usage-guide/topics/ch02-initializing.md) for details.
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_cleanup_thread(void);
