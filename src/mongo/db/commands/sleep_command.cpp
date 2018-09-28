/**
*    Copyright (C) 2018 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/util/log.h"

namespace mongo {

/* for diagnostic / testing purposes. Enabled via command line. */
class CmdSleep : public BasicCommand {
public:
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "internal testing command. Run a no-op command for an arbitrary amount of time. "
               "If neither 'secs' nor 'millis' is set, command will sleep for 10 seconds. "
               "If both are set, command will sleep for the sum of 'secs' and 'millis.'\n"
               "   w:<bool> (deprecated: use 'lock' instead) if true, takes a write lock.\n"
               "   lock: r, w, none. If r or w, db will block under a lock. Defaults to r."
               " 'lock' and 'w' may not both be set.\n"
               "   secs:<seconds> Amount of time to sleep, in seconds.\n"
               "   millis:<milliseconds> Amount of time to sleep, in ms.\n";
    }

    // No auth needed because it only works when enabled via command line.
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {}

    void _sleepInReadLock(mongo::OperationContext* opCtx, long long millis) {
        Lock::GlobalRead lk(opCtx);
        opCtx->sleepFor(Milliseconds(millis));
    }

    void _sleepInWriteLock(mongo::OperationContext* opCtx, long long millis) {
        Lock::GlobalWrite lk(opCtx);
        opCtx->sleepFor(Milliseconds(millis));
    }

    CmdSleep() : BasicCommand("sleep") {}
    bool run(OperationContext* opCtx,
             const std::string& ns,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        log() << "test only command sleep invoked";
        long long millis = 0;

        if (cmdObj["secs"] || cmdObj["millis"]) {
            if (cmdObj["secs"]) {
                uassert(34344, "'secs' must be a number.", cmdObj["secs"].isNumber());
                millis += cmdObj["secs"].numberLong() * 1000;
            }
            if (cmdObj["millis"]) {
                uassert(34345, "'millis' must be a number.", cmdObj["millis"].isNumber());
                millis += cmdObj["millis"].numberLong();
            }
        } else {
            millis = 10 * 1000;
        }

        if (!cmdObj["lock"]) {
            // Legacy implementation
            if (cmdObj.getBoolField("w")) {
                _sleepInWriteLock(opCtx, millis);
            } else {
                _sleepInReadLock(opCtx, millis);
            }
        } else {
            uassert(34346, "Only one of 'w' and 'lock' may be set.", !cmdObj["w"]);

            std::string lock(cmdObj.getStringField("lock"));
            if (lock == "none") {
                opCtx->sleepFor(Milliseconds(millis));
            } else if (lock == "w") {
                _sleepInWriteLock(opCtx, millis);
            } else {
                uassert(34347, "'lock' must be one of 'r', 'w', 'none'.", lock == "r");
                _sleepInReadLock(opCtx, millis);
            }
        }

        // Interrupt point for testing (e.g. maxTimeMS).
        opCtx->checkForInterrupt();

        return true;
    }
};

MONGO_REGISTER_TEST_COMMAND(CmdSleep);
}  // namespace
