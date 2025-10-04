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

/*
 * Linux doesn't expose kTLS headers in its uapi. Its possible to get these headers
 * via glibc but support can vary depending on the version of glibc on the host.
 * Instead we define linux specific values inline.
 *
 * - https://elixir.bootlin.com/linux/v6.3.8/A/ident/TCP_ULP
 * - https://elixir.bootlin.com/linux/v6.3.8/A/ident/SOL_TCP
 */

#if defined(S2N_KTLS_SUPPORTED)
    #include <linux/tls.h>

    /* socket definitions */
    #define S2N_TCP_ULP 31 /* Attach a ULP to a TCP connection.  */
    #define S2N_SOL_TCP 6  /* TCP level */
    #define S2N_SOL_TLS 282

    /* We typically only define values not available in the linux uapi. However,
     * only TLS_TX is defined in the first version of kTLS. Since calling setsockopt
     * with TLS_RX fails and is non destructive, define both TX and RX to keep the
     * definitions co-located and avoid extra ifdefs.
     * https://github.com/torvalds/linux/blob/3c4d7559159bfe1e3b94df3a657b2cda3a34e218/include/uapi/linux/tls.h#L43
     */
    #define S2N_TLS_TX 1
    #define S2N_TLS_RX 2

    #define S2N_TLS_SET_RECORD_TYPE TLS_SET_RECORD_TYPE
    #define S2N_TLS_GET_RECORD_TYPE TLS_GET_RECORD_TYPE
#else
    /* For unsupported platforms 0-init (array of size 1) all values. */

    /* socket definitions */
    #define S2N_TCP_ULP 0
    #define S2N_SOL_TCP 0
    #define S2N_SOL_TLS 0

    #define S2N_TLS_TX 0
    #define S2N_TLS_RX 0

    #define S2N_TLS_SET_RECORD_TYPE 0
    #define S2N_TLS_GET_RECORD_TYPE 0
#endif

/* Common */
#define S2N_TLS_ULP_NAME      "tls"
#define S2N_TLS_ULP_NAME_SIZE sizeof(S2N_TLS_ULP_NAME)
