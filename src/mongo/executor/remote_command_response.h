// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/rpc/message.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace rpc {
class ReplyInterface;
}  // namespace rpc

namespace [[MONGO_MOD_PUBLIC]] executor {

std::vector<std::string> extractErrorLabels(BSONObj data);
boost::optional<Milliseconds> extractBaseBackoffMS(BSONObj data);

/**
 * Type of object describing the response of previously sent RemoteCommandRequest.
 *
 * The target member may be used by callers to associate a HostAndPort with the remote or
 * local error that the RemoteCommandResponse holds. The "status" member will be populated
 * with possible local errors, while the "data" member may hold any remote errors.
 *
 */
struct RemoteCommandResponse {
    RemoteCommandResponse() = default;

    RemoteCommandResponse(HostAndPort hp, Status s);

    RemoteCommandResponse(HostAndPort hp, Status s, Microseconds elapsed);

    RemoteCommandResponse(HostAndPort hp,
                          BSONObj dataObj,
                          Microseconds elapsed,
                          bool moreToCome = false);

    std::string toString() const;
    std::vector<std::string> getErrorLabels() const;
    boost::optional<Milliseconds> getBaseBackoffMS() const;

    bool operator==(const RemoteCommandResponse& rhs) const;
    bool operator!=(const RemoteCommandResponse& rhs) const;

    bool isOK() const;

    [[MONGO_MOD_PUBLIC]] static RemoteCommandResponse make_forTest(Status s);
    [[MONGO_MOD_PUBLIC]] static RemoteCommandResponse make_forTest(Status s, Microseconds elapsed);
    [[MONGO_MOD_PUBLIC]] static RemoteCommandResponse make_forTest(BSONObj dataObj,
                                                                   Microseconds elapsed,
                                                                   bool moreToCome = false);

    BSONObj data;  ///< Always owned. May point into message.
    boost::optional<Microseconds> elapsed;
    /**
     * `status` is the `Status` of sending the request and receiving the response. It is _not_ a
     * status associated with the response payload, e.g. it does not indicate whether the response
     * payload has an `ok: 1.0` field.
     */
    Status status = Status::OK();
    /** `moreToCome` indicates whether the moreToCome bit is set on an exhaust message. */
    bool moreToCome = false;
    HostAndPort target;

    friend std::ostream& operator<<(std::ostream& os, const RemoteCommandResponse& request);

private:
    RemoteCommandResponse(Status s);
    RemoteCommandResponse(Status s, Microseconds elapsed);
    RemoteCommandResponse(BSONObj dataObj, Microseconds elapsed, bool moreToCome);
};
}  // namespace executor
}  // namespace mongo
