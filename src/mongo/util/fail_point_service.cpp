
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/util/fail_point_service.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

using std::unique_ptr;

MONGO_FAIL_POINT_DEFINE(dummy);  // used by tests in jstests/fail_point

unique_ptr<FailPointRegistry> _fpRegistry(nullptr);

MONGO_INITIALIZER_GENERAL(FailPointRegistry,
                          MONGO_NO_PREREQUISITES,
                          ("BeginGeneralStartupOptionRegistration"))
(InitializerContext* context) {
    _fpRegistry.reset(new FailPointRegistry());
    return Status::OK();
}

MONGO_INITIALIZER_GENERAL(AllFailPointsRegistered, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS)
(InitializerContext* context) {
    _fpRegistry->freeze();
    return Status::OK();
}

FailPointRegistry* getGlobalFailPointRegistry() {
    return _fpRegistry.get();
}

void setGlobalFailPoint(const std::string& failPointName, const BSONObj& cmdObj) {
    FailPointRegistry* registry = getGlobalFailPointRegistry();
    FailPoint* failPoint = registry->getFailPoint(failPointName);

    if (failPoint == nullptr)
        uasserted(ErrorCodes::FailPointSetFailed, failPointName + " not found");

    FailPoint::Mode mode;
    FailPoint::ValType val;
    BSONObj data;
    std::tie(mode, val, data) = uassertStatusOK(FailPoint::parseBSON(cmdObj));

    failPoint->setMode(mode, val, data);
    warning() << "failpoint: " << failPointName << " set to: " << failPoint->toBSON();
}

FailPointEnableBlock::FailPointEnableBlock(const std::string& failPointName) {
    _failPoint = getGlobalFailPointRegistry()->getFailPoint(failPointName);
    invariant(_failPoint != nullptr);
    _failPoint->setMode(FailPoint::alwaysOn);
}

FailPointEnableBlock::~FailPointEnableBlock() {
    _failPoint->setMode(FailPoint::off);
}

}  // namespace mongo
