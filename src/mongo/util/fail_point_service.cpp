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

MONGO_FAIL_POINT_DEFINE(dummy);  // used by tests in jstests/fail_point

MONGO_INITIALIZER_GROUP(FailPointRegistry, (), ("BeginStartupOptionHandling"));

MONGO_INITIALIZER_GENERAL(AllFailPointsRegistered, (), ())
(InitializerContext* context) {
    globalFailPointRegistry().freeze();
    return Status::OK();
}

FailPointRegistry& globalFailPointRegistry() {
    static auto& p = *new FailPointRegistry();
    return p;
}

void setGlobalFailPoint(const std::string& failPointName, const BSONObj& cmdObj) {
    FailPoint* failPoint = globalFailPointRegistry().find(failPointName);

    if (failPoint == nullptr)
        uasserted(ErrorCodes::FailPointSetFailed, failPointName + " not found");

    failPoint->setMode(uassertStatusOK(FailPoint::parseBSON(cmdObj)));
    warning() << "failpoint: " << failPointName << " set to: " << failPoint->toBSON();
}

FailPointEnableBlock::FailPointEnableBlock(const std::string& failPointName)
    : _failPointName(failPointName) {
    _failPoint = globalFailPointRegistry().find(failPointName);
    invariant(_failPoint != nullptr);
    _failPoint->setMode(FailPoint::alwaysOn);
    warning() << "failpoint: " << failPointName << " set to: " << _failPoint->toBSON();
}

FailPointEnableBlock::FailPointEnableBlock(const std::string& failPointName, const BSONObj& data)
    : _failPointName(failPointName) {
    _failPoint = globalFailPointRegistry().find(failPointName);
    invariant(_failPoint != nullptr);
    _failPoint->setMode(FailPoint::alwaysOn, 0, data);
    warning() << "failpoint: " << failPointName << " set to: " << _failPoint->toBSON();
}


FailPointEnableBlock::~FailPointEnableBlock() {
    _failPoint->setMode(FailPoint::off);
    warning() << "failpoint: " << _failPointName << " set to: " << _failPoint->toBSON();
}

}  // namespace mongo
