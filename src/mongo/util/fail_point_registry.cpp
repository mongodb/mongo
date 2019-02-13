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

#include "mongo/util/fail_point_registry.h"

#include "mongo/bson/json.h"
#include "mongo/util/fail_point_server_parameter_gen.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

constexpr auto kFailPointServerParameterPrefix = "failpoint."_sd;

FailPointRegistry::FailPointRegistry() : _frozen(false) {}

Status FailPointRegistry::addFailPoint(const std::string& name, FailPoint* failPoint) {
    if (_frozen) {
        return {ErrorCodes::CannotMutateObject, "Registry is already frozen"};
    }

    if (_fpMap.count(name) > 0) {
        return {ErrorCodes::Error(51006),
                str::stream() << "Fail point already registered: " << name};
    }

    _fpMap.insert(make_pair(name, failPoint));
    return Status::OK();
}

FailPoint* FailPointRegistry::getFailPoint(const std::string& name) const {
    return mapFindWithDefault(_fpMap, name, static_cast<FailPoint*>(nullptr));
}

void FailPointRegistry::freeze() {
    _frozen = true;
}

void FailPointRegistry::registerAllFailPointsAsServerParameters() {
    for (const auto& it : _fpMap) {
        // Intentionally leaked.
        new FailPointServerParameter(it.first, ServerParameterType::kStartupOnly);
    }
}

FailPointServerParameter::FailPointServerParameter(StringData name, ServerParameterType spt)
    : ServerParameter(kFailPointServerParameterPrefix.toString() + name.toString(), spt),
      _data(getGlobalFailPointRegistry()->getFailPoint(name.toString())) {
    invariant(name != "failpoint.*", "Failpoint prototype was auto-registered from IDL");
    invariant(_data != nullptr, str::stream() << "Unknown failpoint: " << name);
}

void FailPointServerParameter::append(OperationContext* opCtx,
                                      BSONObjBuilder& b,
                                      const std::string& name) {
    b << name << _data->toBSON();
}

Status FailPointServerParameter::setFromString(const std::string& str) {
    BSONObj failPointOptions;
    try {
        failPointOptions = fromjson(str);
    } catch (DBException& ex) {
        return ex.toStatus();
    }

    auto swParsedOptions = FailPoint::parseBSON(failPointOptions);
    if (!swParsedOptions.isOK()) {
        return swParsedOptions.getStatus();
    }

    FailPoint::Mode mode;
    FailPoint::ValType val;
    BSONObj data;
    std::tie(mode, val, data) = std::move(swParsedOptions.getValue());

    _data->setMode(mode, val, data);

    return Status::OK();
}
}  // namespace mongo
