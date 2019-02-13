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
#include "mongo/db/commands.h"

namespace mongo {
/**
 * An abstract base class which implements all functionality common to the mongoD and mongoS
 * 'profile' command, and defines a number of virtual functions through which it delegates any
 * op-specific work to its derived classes.
 */
class ProfileCmdBase : public BasicCommand {
public:
    ProfileCmdBase() : BasicCommand("profile") {}
    virtual ~ProfileCmdBase() {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const final {
        return "controls the behaviour of the performance profiler, the fraction of eligible "
               "operations which are sampled for logging/profiling, and the threshold duration at "
               "which ops become eligible. See "
               "http://docs.mongodb.org/manual/reference/command/profile";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const final;

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final;

protected:
    // Applies the given profiling level, or throws if the profiling level could not be set. On
    // success, returns an integer indicating the previous profiling level.
    virtual int _applyProfilingLevel(OperationContext* opCtx,
                                     const std::string& dbName,
                                     int profilingLevel) const = 0;
};
}  // namespace mongo
