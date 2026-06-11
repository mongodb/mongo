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
 * @file async_offload.h
 * 
 * The following APIs enable applications to offload expensive handshake operations that do not require user input.
 * This model can be useful to move CPU-heavy operations (e.g. cryptographic calculations) out of the main event loop.
 */

/**
 * Opaque struct for the async offloading operation
 */
struct s2n_async_offload_op;

/**
 * The type of operations supported by the async offloading callback. Each type is represented by a different bit.
 * 
 * S2N_ASYNC_OFFLOAD_ALLOW_ALL will automatically opt in to all the new types added in the future.
 */
typedef enum {
    S2N_ASYNC_OFFLOAD_PKEY_VERIFY = 0x01,
    /* Max value: ISO C restricts enumerator values to range of ‘int’ before C2X. */
    S2N_ASYNC_OFFLOAD_ALLOW_ALL = 0x7FFFFFFF,
} s2n_async_offload_op_type;

/**
 * The callback function invoked every time an allowed async operation is encountered during the handshake.
 * 
 * To perform an operation asynchronously, the following condiditions must be satisfied:
 * 1) This op type must be included in the allow list;
 * 2) Async offloading callback returns success and s2n_async_offload_op_perform() is invoked outside the callback.
 *
 * If s2n_async_offload_op_perform() is invoked inside the callback, it is equivalent to the synchronous use case.
 * 
 * `op` is owned by s2n-tls and will be freed along with s2n_connection eventually.
 *
 * @param conn Connection which triggered the async offloading callback
 * @param op An opaque object representing the async operation
 * @param ctx Application data provided to the callback via s2n_config_set_async_offload_callback()
 */
typedef int (*s2n_async_offload_cb)(struct s2n_connection *conn, struct s2n_async_offload_op *op, void *ctx);

/**
 * Sets up the custom async offloading callback and configures the offloaded handshake operations via allow_list.
 * 
 * The value of allow_list should be the Bit-OR of all the allowed s2n_async_offload_op_type values.
 * 
 * S2N_ASYNC_OFFLOAD_ALLOW_ALL provides the performance benefit of offloading all the supported operations;
 * ensure your callback can support arbitrary operations. Otherwise, only allow operations that fit your use case.
 *
 * @param config Config to set the callback
 * @param allow_list A bit representation of allowed operations
 * @param fn The function that should be called for each allowed async operation
 * @param ctx Optional application data passed to the callback
 */
S2N_API extern int s2n_config_set_async_offload_callback(struct s2n_config *config, uint32_t allow_list,
        s2n_async_offload_cb fn, void *ctx);

/**
 * Performs the operation triggered by the async offloading callback.
 * 
 * To execute operations asynchronously, users should spawn a separate thread to invoke s2n_async_offload_op_perform()
 * and immediately return S2N_SUCCESS from the callback without waiting for that separate thread to complete.
 * 
 * s2n_negotiate() will throw an `S2N_ERR_T_BLOCKED` error if the handshake is pending on the async offloading callback.
 * Retrying s2n_negotiate() will produce the same result until s2n_async_offload_op_perform() is completed.
 * 
 * s2n_async_offload_op_perform() can only be called once for each triggered operation.
 * 
 * @param op An opaque object representing the async operation
 */
S2N_API extern int s2n_async_offload_op_perform(struct s2n_async_offload_op *op);
