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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/util/fail_point_server_parameter.h"

#include "mongo/base/status.h"
#include "mongo/bson/json.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/fail_point_registry.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {

const std::string FailPointServerParameter::failPointPrefix = "failpoint.";

FailPointServerParameter::FailPointServerParameter(std::string name, FailPoint* failpoint)
    : ServerParameter(ServerParameterSet::getGlobal(),
                      failPointPrefix + name,
                      true /* allowedToChangeAtStartup */,
                      false /* allowedToChangeAtRuntime */),
      _failpoint(failpoint),
      _failPointName(name) {}

void FailPointServerParameter::append(OperationContext* txn,
                                      BSONObjBuilder& b,
                                      const std::string& name) {
    b << name << _failpoint->toBSON();
}

Status FailPointServerParameter::set(const BSONElement& newValueElement) {
    return {ErrorCodes::InternalError,
            "FailPointServerParameter::setFromString() should be used instead of "
            "FailPointServerParameter::set()"};
}

Status FailPointServerParameter::setFromString(const std::string& str) {
    FailPointRegistry* registry = getGlobalFailPointRegistry();
    FailPoint* failPoint = registry->getFailPoint(_failPointName);
    if (failPoint == NULL) {
        return {ErrorCodes::BadValue,
                str::stream() << _failPointName << " not found in fail point registry"};
    }

    BSONObj failPointOptions;
    try {
        failPointOptions = fromjson(str);
    } catch (DBException& ex) {
        return ex.toStatus();
    }

    FailPoint::Mode mode;
    FailPoint::ValType val;
    BSONObj data;
    auto swParsedOptions = FailPoint::parseBSON(failPointOptions);
    if (!swParsedOptions.isOK()) {
        return swParsedOptions.getStatus();
    }
    std::tie(mode, val, data) = std::move(swParsedOptions.getValue());

    failPoint->setMode(mode, val, data);

    return Status::OK();
}

}  // namespace mongo
