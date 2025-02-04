#ifndef AWS_COMMON_PRIVATE_EXTERNAL_MODULE_IMPL_H
#define AWS_COMMON_PRIVATE_EXTERNAL_MODULE_IMPL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>

/**
 * Initializes the JSON module for use.
 * @param allocator The allocator to use for creating aws_json_value structs.
 */
void aws_json_module_init(struct aws_allocator *allocator);

/**
 * Cleans up the JSON module. Should be called when finished using the module.
 */
void aws_json_module_cleanup(void);

void aws_cbor_module_init(struct aws_allocator *allocator);

void aws_cbor_module_cleanup(void);

#endif // AWS_COMMON_PRIVATE_EXTERNAL_MODULE_IMPL_H
