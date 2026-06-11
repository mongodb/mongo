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
 * @file npn.h
 *
 * The Next Protocol Negotiation Extension, or NPN, was an RFC proposal to
 * negotiate an application protocol. This proposal was never standardized, and
 * it was eventually replaced with the ALPN Extension. However, an early draft
 * version of the NPN Extension was implemented in Openssl.
 * Now, OpenSSL clients and servers may require this extension in order to connect.
 *
 * s2n-tls supports NPN to make it easier for users whose peers require this
 * extension, but s2n-tls does NOT recommend its use. The specific draft version
 * supported is https://datatracker.ietf.org/doc/html/draft-agl-tls-nextprotoneg-03,
 * which provides interoperability with OpenSSL.
 */

/**
 * Turns on support for the NPN extension.
 * 
 * This will allow an s2n-tls client to send the NPN extension and an s2n-tls
 * server to respond to receiving the NPN extension. However, if their peer
 * also indicates support for the ALPN extension, s2n-tls will prefer that.
 * 
 * Use s2n_config_append_protocol_preference() to set up a list of supported protocols.
 * After the negotiation for the connection has completed, the agreed-upon protocol
 * can be retrieved with s2n_get_application_protocol().
 *
 * @param config A pointer to the config object
 * @param enable Set to true to enable. Set to false to disable.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on error.
 */
S2N_API int s2n_config_set_npn(struct s2n_config *config, bool enable);
