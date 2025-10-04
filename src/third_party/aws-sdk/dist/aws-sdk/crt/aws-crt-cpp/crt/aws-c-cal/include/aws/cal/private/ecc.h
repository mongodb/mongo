#ifndef AWS_C_CAL_PRIVATE_ECC_H
#define AWS_C_CAL_PRIVATE_ECC_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cal/ecc.h>

#include <aws/common/byte_buf.h>

struct aws_der_decoder;

AWS_EXTERN_C_BEGIN

AWS_CAL_API int aws_der_decoder_load_ecc_key_pair(
    struct aws_der_decoder *decoder,
    struct aws_byte_cursor *out_public_x_coor,
    struct aws_byte_cursor *out_public_y_coor,
    struct aws_byte_cursor *out_private_d,
    enum aws_ecc_curve_name *out_curve_name);

AWS_EXTERN_C_END

#endif /* AWS_C_CAL_PRIVATE_ECC_H */
