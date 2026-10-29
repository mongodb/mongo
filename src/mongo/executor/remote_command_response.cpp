// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/remote_command_response.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace executor {
using namespace std::literals::string_view_literals;

RemoteCommandResponse::RemoteCommandResponse(HostAndPort hp, Status s)
    : status(std::move(s)), target(std::move(hp)) {
    invariant(!isOK());
}

RemoteCommandResponse::RemoteCommandResponse(HostAndPort hp, Status s, Microseconds elapsed)
    : elapsed(elapsed.count() == 0 ? boost::none : boost::make_optional(elapsed)),
      status(std::move(s)),
      target(std::move(hp)) {
    invariant(!isOK());
}

RemoteCommandResponse::RemoteCommandResponse(HostAndPort hp,
                                             BSONObj dataObj,
                                             Microseconds elapsed,
                                             bool moreToCome)
    : data(std::move(dataObj)), elapsed(elapsed), moreToCome(moreToCome), target(std::move(hp)) {
    // The buffer backing the default empty BSONObj has static duration so it is effectively
    // owned.
    invariant(data.isOwned() || data.objdata() == BSONObj().objdata());
}

std::string RemoteCommandResponse::toString() const {
    return fmt::format(
        "RemoteResponse -- "
        " cmd: {}"
        " target: {}"
        " status: {}"
        " elapsedMicros: {}"
        " moreToCome: {}",
        data.toString(),
        target.toString(),
        status.toString(),
        elapsed ? std::string_view(elapsed->toString()) : "n/a"sv,
        moreToCome);
}

bool RemoteCommandResponse::isOK() const {
    return status.isOK();
}

std::vector<std::string> extractErrorLabels(BSONObj data) {
    if (BSONElement errorLabelsElement = data["errorLabels"]; !errorLabelsElement.eoo()) {
        auto errorLabelsArray = errorLabelsElement.Array();

        std::vector<std::string> errorLabels{};
        errorLabels.resize(errorLabelsArray.size());

        std::ranges::transform(errorLabelsArray, errorLabels.begin(), [](const BSONElement& data) {
            return data.String();
        });

        return errorLabels;
    }

    return {};
}

std::vector<std::string> RemoteCommandResponse::getErrorLabels() const {
    if (!status.isOK()) {
        return {};
    }

    return extractErrorLabels(data);
}

boost::optional<Milliseconds> extractBaseBackoffMS(BSONObj data) {
    if (BSONElement retryAfterElement = data["baseBackoffMS"]; !retryAfterElement.eoo()) {
        if (!retryAfterElement.isNumber()) {
            return boost::none;
        }
        return Milliseconds{retryAfterElement.safeNumberLong()};
    }

    return boost::none;
}

boost::optional<Milliseconds> RemoteCommandResponse::getBaseBackoffMS() const {
    if (!status.isOK()) {
        return boost::none;
    }

    return extractBaseBackoffMS(data);
}

bool RemoteCommandResponse::operator==(const RemoteCommandResponse& rhs) const {
    if (this == &rhs) {
        return true;
    }
    SimpleBSONObjComparator bsonComparator;
    return bsonComparator.evaluate(data == rhs.data) && elapsed == rhs.elapsed;
}

bool RemoteCommandResponse::operator!=(const RemoteCommandResponse& rhs) const {
    return !(*this == rhs);
}

std::ostream& operator<<(std::ostream& os, const RemoteCommandResponse& response) {
    return os << response.toString();
}

RemoteCommandResponse RemoteCommandResponse::make_forTest(Status s) {
    return RemoteCommandResponse(std::move(s));
}

RemoteCommandResponse RemoteCommandResponse::make_forTest(Status s, Microseconds elapsed) {
    return RemoteCommandResponse(std::move(s), elapsed);
}

RemoteCommandResponse RemoteCommandResponse::make_forTest(BSONObj dataObj,
                                                          Microseconds elapsed,
                                                          bool moreToCome) {
    return RemoteCommandResponse(std::move(dataObj), elapsed, moreToCome);
}

RemoteCommandResponse::RemoteCommandResponse(Status s) : status(std::move(s)) {
    invariant(!isOK());
}

RemoteCommandResponse::RemoteCommandResponse(Status s, Microseconds elapsed)
    : elapsed(elapsed.count() == 0 ? boost::none : boost::make_optional(elapsed)),
      status(std::move(s)) {
    invariant(!isOK());
}

RemoteCommandResponse::RemoteCommandResponse(BSONObj dataObj, Microseconds elapsed, bool moreToCome)
    : data(std::move(dataObj)), elapsed(elapsed), moreToCome(moreToCome) {}


}  // namespace executor
}  // namespace mongo
