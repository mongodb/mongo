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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObj;

struct HostAndPort;
class Status;
template <typename T>
class StatusWith;
class StringData;

struct HostSettings {
    enum class State { kForward, kHangUp, kDiscard };

    State state = State::kForward;
    Milliseconds delay{0};
    double loss = 0.0;
};

using HostSettingsMap = stdx::unordered_map<HostAndPort, HostSettings>;

class BridgeCommand {
public:
    static StatusWith<BridgeCommand*> findCommand(StringData cmdName);

    virtual ~BridgeCommand() = 0;

    virtual Status run(const BSONObj& cmdObj,
                       stdx::mutex* settingsMutex,
                       HostSettingsMap* settings) = 0;
};

}  // namespace mongo
