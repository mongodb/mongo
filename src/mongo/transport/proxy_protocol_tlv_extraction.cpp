/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#include "mongo/transport/proxy_protocol_tlv_extraction.h"

#include "mongo/config.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/ssl_peer_info.h"

#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl_manager.h"
#endif

#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport {

void applyProxyProtocolTlvs(const ParserResults& results, const std::shared_ptr<Session>& session) {
    // Extract SNI from top-level TLVs.
    boost::optional<std::string> sni;
    for (const auto& tlv : results.tlvs) {
        if (tlv.type == kProxyProtocolTypeAuthority) {
            sni = tlv.data;
            LOGV2_DEBUG(11973600, 4, "Parsed SNI from proxy protocol header", "sni"_attr = sni);
            break;
        }
    }

#ifdef MONGO_CONFIG_SSL
    {
        SSLX509Name dn;
        stdx::unordered_set<RoleName> parsedRoles;

        // Extract the DN and roles from the proxy protocol SSL sub-TLVs.
        if (results.sslTlvs) {
            for (const auto& subTLV : results.sslTlvs->subTLVs) {
                if (subTLV.type == kProxyProtocolSSLTlvDN) {
                    auto swDN = parseDN(subTLV.data);
                    uassertStatusOK(swDN);
                    dn = std::move(swDN.getValue());
                } else if (subTLV.type == kProxyProtocolSSLTlvPeerRoles) {
                    ConstDataRange rolesCDR(subTLV.data);
                    auto swRoles = parsePeerRoles(rolesCDR);
                    uassertStatusOK(swRoles);
                    parsedRoles = std::move(swRoles.getValue());
                }
            }
        }

        // Set the SSLPeerInfo fields based on the parsed TLV data.
        if (sni || !dn.empty() || !parsedRoles.empty()) {
            uassert(ErrorCodes::BadValue,
                    "Proxy protocol header contains roles without a subject DN",
                    parsedRoles.empty() || !dn.empty());
            auto& sslPeerInfo = SSLPeerInfo::forSession(session);
            uassert(11793600,
                    "SSLPeerInfo is not empty during proxy protocol header parsing",
                    !sslPeerInfo);
            // Always set the isTLS flag to false here because the actual ingress connection
            // from the proxy protocol header is not a TLS connection, even if the original client
            // used TLS to connect to the proxy
            sslPeerInfo = std::make_shared<SSLPeerInfo>(
                false, std::move(dn), std::move(sni), std::move(parsedRoles));
        }
    }
#endif
}

}  // namespace mongo::transport

