/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/request_types/control_balancer_request_type.h"

#include <string>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

const char kControlBalancer[] = "controlBalancer";
const char kConfigControlBalancer[] = "_configsvrControlBalancer";
const char kStartAction[] = "start";
const char kStopAction[] = "stop";

}  // namespace

ControlBalancerRequest::ControlBalancerRequest(BalancerControlAction action) : _action(action) {}

StatusWith<ControlBalancerRequest> ControlBalancerRequest::parseFromMongosCommand(
    const BSONObj& obj) {
    return _parse(obj, kControlBalancer);
}

StatusWith<ControlBalancerRequest> ControlBalancerRequest::parseFromConfigCommand(
    const BSONObj& obj) {
    return _parse(obj, kConfigControlBalancer);
}

StatusWith<ControlBalancerRequest> ControlBalancerRequest::_parse(const BSONObj& obj,
                                                                  StringData commandString) {
    if (obj.firstElementFieldName() != commandString) {
        return {ErrorCodes::InternalError,
                str::stream() << "Expected to find a " << commandString << " command, but found "
                              << obj};
    }

    std::string actionString;
    Status status = bsonExtractStringField(obj, commandString, &actionString);
    if (!status.isOK()) {
        return status;
    }

    if (actionString == kStartAction)
        return ControlBalancerRequest(kStart);
    if (actionString == kStopAction)
        return ControlBalancerRequest(kStop);

    return {ErrorCodes::IllegalOperation,
            str::stream() << actionString << " is not a valid balancer control action"};
}

BSONObj ControlBalancerRequest::toCommandForConfig() const {
    switch (_action) {
        case kStart:
            return BSON(kConfigControlBalancer << kStartAction);
        case kStop:
            return BSON(kConfigControlBalancer << kStopAction);
    }

    MONGO_UNREACHABLE;
}

}  // namespace mongo
