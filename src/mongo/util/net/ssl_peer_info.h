/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include "mongo/transport/session.h"
#include "mongo/util/net/ssl_types.h"

namespace mongo {
/**
 * Contains information extracted from the peer certificate which is consumed by subsystems
 * outside of the networking stack.
 */
struct SSLPeerInfo {
    explicit SSLPeerInfo(SSLX509Name subjectName,
                         boost::optional<std::string> sniName = {},
                         stdx::unordered_set<RoleName> roles = {})
        : isTLS(true),
          subjectName(std::move(subjectName)),
          sniName(std::move(sniName)),
          roles(std::move(roles)) {}
    SSLPeerInfo() = default;

    explicit SSLPeerInfo(boost::optional<std::string> sniName)
        : isTLS(true), sniName(std::move(sniName)) {}

    /**
     * This flag is used to indicate if the underlying socket is using TLS or not. A default
     * constructor of SSLPeerInfo indicates that TLS is not being used, and the other
     * constructors set its value to true.
     */
    bool isTLS = false;

    SSLX509Name subjectName;
    boost::optional<std::string> sniName;
    stdx::unordered_set<RoleName> roles;

    static SSLPeerInfo& forSession(const transport::SessionHandle& session);
    static const SSLPeerInfo& forSession(const transport::ConstSessionHandle& session);
};
}  // namespace mongo