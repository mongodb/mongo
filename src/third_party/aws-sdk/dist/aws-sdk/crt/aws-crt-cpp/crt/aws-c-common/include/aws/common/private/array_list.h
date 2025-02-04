#ifndef AWS_COMMON_PRIVATE_ARRAY_LIST_H
#define AWS_COMMON_PRIVATE_ARRAY_LIST_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

AWS_EXTERN_C_BEGIN

/**
 * Helper function that calculates the number of bytes needed by an array_list, where "index" is the last valid
 * index.
 */
int aws_array_list_calc_necessary_size(struct aws_array_list *AWS_RESTRICT list, size_t index, size_t *necessary_size);

AWS_EXTERN_C_END

#endif /* AWS_COMMON_PRIVATE_ARRAY_LIST_H */
