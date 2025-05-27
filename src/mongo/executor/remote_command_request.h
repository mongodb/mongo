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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/metadata.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <type_traits>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace executor {

struct RemoteCommandRequest {

    // Indicates that there is no timeout for the request to complete
    static constexpr Milliseconds kNoTimeout{-1};

    // Type to represent the internal id of this request
    typedef uint64_t RequestId;

    RemoteCommandRequest();

    RemoteCommandRequest(RequestId requestId,
                         const HostAndPort& hostAndPort,
                         const DatabaseName& dbName,
                         const BSONObj& cmdObj,
                         const BSONObj& metadataObj,
                         OperationContext* opCtx,
                         Milliseconds timeoutMillis = kNoTimeout,
                         bool fireAndForget = false,
                         boost::optional<UUID> operationKey = boost::none);

    RemoteCommandRequest(const HostAndPort& target,
                         const DatabaseName& dbName,
                         const BSONObj& cmdObj,
                         const BSONObj& metadataObj,
                         OperationContext* opCtx,
                         Milliseconds timeoutMillis = kNoTimeout,
                         bool fireAndForget = false,
                         boost::optional<UUID> operationKey = boost::none);

    RemoteCommandRequest(const HostAndPort& target,
                         const DatabaseName& dbName,
                         const BSONObj& cmdObj,
                         const BSONObj& metadataObj,
                         OperationContext* opCtx,
                         bool fireAndForget,
                         boost::optional<UUID> operationKey = boost::none)
        : RemoteCommandRequest(
              target, dbName, cmdObj, metadataObj, opCtx, kNoTimeout, fireAndForget, operationKey) {
    }


    RemoteCommandRequest(const HostAndPort& target,
                         const DatabaseName& dbName,
                         const BSONObj& cmdObj,
                         OperationContext* opCtx,
                         Milliseconds timeoutMillis = kNoTimeout,
                         bool fireAndForget = false,
                         boost::optional<UUID> operationKey = boost::none)
        : RemoteCommandRequest(target,
                               dbName,
                               cmdObj,
                               rpc::makeEmptyMetadata(),
                               opCtx,
                               timeoutMillis,
                               fireAndForget,
                               operationKey) {}

    /**
     * Conversion function that performs the RemoteCommandRequest conversion into OpMsgRequest
     */
    explicit operator OpMsgRequest() const;

    std::string toString() const;

    bool operator==(const RemoteCommandRequest& rhs) const;
    bool operator!=(const RemoteCommandRequest& rhs) const;

    friend std::ostream& operator<<(std::ostream& os, const RemoteCommandRequest& response) {
        return (os << response.toString());
    }

    // Internal id of this request. Not interpreted and used for tracing purposes only.
    RequestId id;

    HostAndPort target;

    DatabaseName dbname;
    BSONObj cmdObj;
    BSONObj metadata{rpc::makeEmptyMetadata()};

    // OperationContext is added to each request to allow OP_Command metadata attachment access to
    // the Client object. The OperationContext is only accessed on the thread that calls
    // NetworkInterface::startCommand. It is not safe to access from a thread that does not own the
    // OperationContext in the general case. OperationContext should be non-null on
    // NetworkInterfaces that do user work (i.e. reads, and writes) so that audit and client
    // metadata is propagated. It is allowed to be null if used on NetworkInterfaces without
    // metadata attachment (i.e., replication).
    OperationContext* opCtx{nullptr};

    Milliseconds timeout = kNoTimeout;
    boost::optional<ErrorCodes::Error> timeoutCode;

    bool fireAndForget = false;

    boost::optional<UUID> operationKey;

    // When false, the network interface will refrain from enforcing the 'timeout' for this request,
    // but will still pass the timeout on as maxTimeMSOpOnly.
    bool enforceLocalTimeout = true;

    // Time when the request was scheduled.
    boost::optional<Date_t> dateScheduled;

    transport::ConnectSSLMode sslMode = transport::kGlobalSSLMode;

private:
    /**
     * Sets 'timeout' to the min of the current 'timeout' value and the remaining time on the OpCtx.
     * If the remaining time is less than the provided 'timeout', remembers the timeout error code
     * from the opCtx to use later if the timeout is indeed triggered.  This is important so that
     * timeouts that are a direct result of a user-provided maxTimeMS return MaxTimeMSExpired rather
     * than NetworkInterfaceExceededTimeLimit.
     */
    void _updateTimeoutFromOpCtxDeadline(const OperationContext* opCtx);
};

}  // namespace executor
}  // namespace mongo
