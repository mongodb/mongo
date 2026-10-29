// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/transport/proxy_protocol_header_parser.h"
#include "mongo/transport/session.h"

#include <memory>

namespace mongo::transport {

/**
 * Extracts SSLPeerInfo fields (SNI, DN, roles) from proxy protocol TLV data
 * and attaches them to the given session.
 *
 * - SNI is extracted from top-level TLVs (kProxyProtocolTypeAuthority).
 * - DN and roles are extracted from SSL sub-TLVs (kProxyProtocolSSLTlvDN,
 *   kProxyProtocolSSLTlvPeerRoles) under MONGO_CONFIG_SSL.
 * - SSLPeerInfo is set on the session only when at least one field is present.
 * - isTLS is always false (proxy protocol arrives over UDS, not TLS).
 * - tasserts if SSLPeerInfo already exists on the session.
 */
void applyProxyProtocolTlvs(const ParserResults& results, const std::shared_ptr<Session>& session);

}  // namespace mongo::transport
