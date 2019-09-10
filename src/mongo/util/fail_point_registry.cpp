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

#include <fmt/format.h>

#include "mongo/bson/json.h"
#include "mongo/util/fail_point_server_parameter_gen.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {

using namespace fmt::literals;

FailPointRegistry::FailPointRegistry() : _frozen(false) {}

Status FailPointRegistry::add(const std::string& name, FailPoint* failPoint) {
    if (_frozen) {
        return {ErrorCodes::CannotMutateObject, "Registry is already frozen"};
    }
    auto [pos, ok] = _fpMap.insert({name, failPoint});
    if (!ok) {
        return {ErrorCodes::Error(51006), "Fail point already registered: {}"_format(name)};
    }
    return Status::OK();
}

FailPoint* FailPointRegistry::find(const std::string& name) const {
    auto iter = _fpMap.find(name);
    return (iter == _fpMap.end()) ? nullptr : iter->second;
}

void FailPointRegistry::freeze() {
    _frozen = true;
}

void FailPointRegistry::registerAllFailPointsAsServerParameters() {
    for (const auto& [name, ptr] : _fpMap) {
        // Intentionally leaked.
        new FailPointServerParameter(name, ServerParameterType::kStartupOnly);
    }
}

static constexpr auto kFailPointServerParameterPrefix = "failpoint."_sd;

FailPointServerParameter::FailPointServerParameter(StringData name, ServerParameterType spt)
    : ServerParameter("{}{}"_format(kFailPointServerParameterPrefix, name), spt),
      _data(globalFailPointRegistry().find(name.toString())) {
    invariant(name != "failpoint.*", "Failpoint prototype was auto-registered from IDL");
    invariant(_data != nullptr, "Unknown failpoint: {}"_format(name));
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
    _data->setMode(std::move(swParsedOptions.getValue()));
    return Status::OK();
}
}  // namespace mongo
