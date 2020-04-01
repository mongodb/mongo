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

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/commands/shutdown.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/repl/replication_coordinator.h"

namespace mongo {
namespace {

class CmdShutdownMongoD : public CmdShutdown {
public:
    std::string help() const override {
        return "shutdown the database.  must be ran against admin db and "
               "either (1) ran from localhost or (2) authenticated. If "
               "this is a primary in a replica set and there is no member "
               "within 10 seconds of its optime, it will not shutdown "
               "without force : true.  You can also specify timeoutSecs : "
               "N to wait N seconds for other members to catch up.";
    }

    virtual bool run(OperationContext* opCtx,
                     const std::string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        bool force = cmdObj.hasField("force") && cmdObj["force"].trueValue();

        long long timeoutSecs = 15;
        if (cmdObj.hasField("timeoutSecs")) {
            timeoutSecs = cmdObj["timeoutSecs"].numberLong();
        }

        // This code may race with a new index build starting up. We may get 0 active index builds
        // from the IndexBuildsCoordinator shutdown to proceed, but there is nothing to prevent a
        // new index build from starting after that check.
        if (!force) {
            auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
            auto numIndexBuilds = indexBuildsCoord->getActiveIndexBuildCount(opCtx);
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    str::stream() << "Index builds in progress while processing shutdown command "
                                     "without {force: true}: "
                                  << numIndexBuilds,
                    numIndexBuilds == 0U);
        }

        try {
            repl::ReplicationCoordinator::get(opCtx)->stepDown(
                opCtx, force, Seconds(timeoutSecs), Seconds(120));
        } catch (const DBException& e) {
            if (e.code() != ErrorCodes::NotMaster) {  // ignore not master
                throw;
            }
        }

        // Never returns
        shutdownHelper(cmdObj);
        return true;
    }

} cmdShutdownMongoD;

}  // namespace
}  // namespace mongo
