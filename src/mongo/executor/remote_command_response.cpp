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

#include "mongo/executor/remote_command_response.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace executor {

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
        elapsed ? StringData(elapsed->toString()) : "n/a"_sd,
        moreToCome);
}

bool RemoteCommandResponse::isOK() const {
    return status.isOK();
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
