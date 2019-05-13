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

#include <iosfwd>
#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/rpc/metadata.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/concepts.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace executor {

struct RemoteCommandRequestBase {
    // Indicates that there is no timeout for the request to complete
    static constexpr Milliseconds kNoTimeout{-1};

    // Indicates that there is no expiration time by when the request needs to complete
    static constexpr Date_t kNoExpirationDate{Date_t::max()};

    // Type to represent the internal id of this request
    typedef uint64_t RequestId;

    RemoteCommandRequestBase();
    RemoteCommandRequestBase(RequestId requestId,
                             const std::string& theDbName,
                             const BSONObj& theCmdObj,
                             const BSONObj& metadataObj,
                             OperationContext* opCtx,
                             Milliseconds timeoutMillis);

    // Internal id of this request. Not interpreted and used for tracing purposes only.
    RequestId id;

    std::string dbname;
    BSONObj metadata{rpc::makeEmptyMetadata()};
    BSONObj cmdObj;

    // OperationContext is added to each request to allow OP_Command metadata attachment access to
    // the Client object. The OperationContext is only accessed on the thread that calls
    // NetworkInterface::startCommand. It is not safe to access from a thread that does not own the
    // OperationContext in the general case. OperationContext should be non-null on
    // NetworkInterfaces that do user work (i.e. reads, and writes) so that audit and client
    // metadata is propagated. It is allowed to be null if used on NetworkInterfaces without
    // metadata attachment (i.e., replication).
    OperationContext* opCtx{nullptr};

    Milliseconds timeout = kNoTimeout;

    // Deadline by when the request must be completed
    Date_t expirationDate = kNoExpirationDate;

    transport::ConnectSSLMode sslMode = transport::kGlobalSSLMode;

protected:
    ~RemoteCommandRequestBase() = default;
};

/**
 * Type of object describing a command to execute against a remote MongoDB node.
 */
template <typename Target>
struct RemoteCommandRequestImpl : RemoteCommandRequestBase {
    RemoteCommandRequestImpl();

    // Allow implicit conversion from RemoteCommandRequest to RemoteCommandRequestOnAny
    REQUIRES_FOR_NON_TEMPLATE(std::is_same_v<Target, std::vector<HostAndPort>>)
    RemoteCommandRequestImpl(const RemoteCommandRequestImpl<HostAndPort>& other)
        : RemoteCommandRequestBase(other), target({other.target}) {}

    // Allow conversion from RemoteCommandRequestOnAny to RemoteCommandRequest with the index of a
    // particular host
    REQUIRES_FOR_NON_TEMPLATE(std::is_same_v<Target, HostAndPort>)
    RemoteCommandRequestImpl(const RemoteCommandRequestImpl<std::vector<HostAndPort>>& other,
                             size_t idx)
        : RemoteCommandRequestBase(other), target(other.target[idx]) {}

    RemoteCommandRequestImpl(RequestId requestId,
                             const Target& theTarget,
                             const std::string& theDbName,
                             const BSONObj& theCmdObj,
                             const BSONObj& metadataObj,
                             OperationContext* opCtx,
                             Milliseconds timeoutMillis);

    RemoteCommandRequestImpl(const Target& theTarget,
                             const std::string& theDbName,
                             const BSONObj& theCmdObj,
                             const BSONObj& metadataObj,
                             OperationContext* opCtx,
                             Milliseconds timeoutMillis = kNoTimeout);

    RemoteCommandRequestImpl(const Target& theTarget,
                             const std::string& theDbName,
                             const BSONObj& theCmdObj,
                             OperationContext* opCtx,
                             Milliseconds timeoutMillis = kNoTimeout)
        : RemoteCommandRequestImpl(
              theTarget, theDbName, theCmdObj, rpc::makeEmptyMetadata(), opCtx, timeoutMillis) {}

    std::string toString() const;

    bool operator==(const RemoteCommandRequestImpl& rhs) const;
    bool operator!=(const RemoteCommandRequestImpl& rhs) const;

    friend std::ostream& operator<<(std::ostream& os, const RemoteCommandRequestImpl& response) {
        return (os << response.toString());
    }

    Target target;
};

extern template struct RemoteCommandRequestImpl<HostAndPort>;
extern template struct RemoteCommandRequestImpl<std::vector<HostAndPort>>;

using RemoteCommandRequest = RemoteCommandRequestImpl<HostAndPort>;
using RemoteCommandRequestOnAny = RemoteCommandRequestImpl<std::vector<HostAndPort>>;

}  // namespace executor
}  // namespace mongo
