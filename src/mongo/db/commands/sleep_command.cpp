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

#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


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
               "   lock: r, ir, w, iw, none. If r or w, db will block under a lock.\n"
               "   If ir or iw, db will block under an intent lock. Defaults to ir."
               " 'lock' and 'w' may not both be set.\n"
               "   secs:<seconds> Amount of time to sleep, in seconds.\n"
               "   millis:<milliseconds> Amount of time to sleep, in ms.\n"
               "'seconds' may be used as an alias for 'secs'.\n";
    }

    // No auth needed because it only works when enabled via command line.
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {}

    /**
     * An empty 'ns' causes the global lock to be taken.
     * A 'ns' that contains <db> causes only the rstl/global/database locks to be taken.
     * A complete 'ns' <coll>.<db> causes the rstl/global/database/collection locks to be taken.
     *
     * Any higher level locks are taken in the appropriate MODE_IS or MODE_IX to match 'mode'.
     */
    void _sleepInLock(mongo::OperationContext* opCtx,
                      long long millis,
                      LockMode mode,
                      const StringData& ns) {
        if (ns.empty()) {
            Lock::GlobalLock lk(opCtx, mode, Date_t::max(), Lock::InterruptBehavior::kThrow);
            LOGV2(6001601,
                  "Global lock acquired by sleep command.",
                  "lockMode"_attr = modeName(mode));
            opCtx->sleepFor(Milliseconds(millis));
            return;
        }
        auto nss = NamespaceString(ns);
        uassert(
            50961, "lockTarget is not a valid namespace", NamespaceString::validDBName(nss.db()));

        auto dbMode = mode;
        if (!nsIsDbOnly(ns)) {
            // Only acquire minimum dbLock mode required for collection lock acquisition.
            dbMode = isSharedLockMode(mode) ? MODE_IS : MODE_IX;
        }

        Lock::DBLock dbLock(opCtx, nss.dbName(), dbMode, Date_t::max());

        if (nsIsDbOnly(ns)) {
            LOGV2(6001602,
                  "Database lock acquired by sleep command.",
                  "lockMode"_attr = modeName(dbMode));
            opCtx->sleepFor(Milliseconds(millis));
            return;
        }

        // Need to acquire DBLock before attempting to acquire a collection lock.
        uassert(50962,
                "lockTarget is not a valid namespace",
                NamespaceString::validCollectionComponent(ns));
        Lock::CollectionLock collLock(opCtx, nss, mode, Date_t::max());
        LOGV2(6001603,
              "Collection lock acquired by sleep command.",
              "lockMode"_attr = modeName(mode));
        opCtx->sleepFor(Milliseconds(millis));
    }

    void _sleepInPBWM(mongo::OperationContext* opCtx, long long millis) {
        Lock::ResourceLock pbwm(opCtx->lockState(), resourceIdParallelBatchWriterMode);
        pbwm.lock(nullptr, MODE_X);
        opCtx->sleepFor(Milliseconds(millis));
        pbwm.unlock();
    }

    void _sleepInRSTL(mongo::OperationContext* opCtx, long long millis) {
        Lock::ResourceLock rstl(opCtx->lockState(), resourceIdReplicationStateTransitionLock);
        rstl.lock(nullptr, MODE_X);
        LOGV2(6001600, "RSTL MODE_X lock acquired by sleep command.");
        opCtx->sleepFor(Milliseconds(millis));
        rstl.unlock();
    }

    CmdSleep() : BasicCommand("sleep") {}
    bool run(OperationContext* opCtx,
             const std::string& ns,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        LOGV2(20504, "Test-only command 'sleep' invoked");
        long long msToSleep = 0;

        if (cmdObj["secs"] || cmdObj["seconds"] || cmdObj["millis"]) {
            uassert(51153,
                    "Only one of 'secs' and 'seconds' may be specified",
                    !(cmdObj["secs"] && cmdObj["seconds"]));

            if (auto secsElem = cmdObj["secs"]) {
                uassert(34344, "'secs' must be a number.", secsElem.isNumber());
                msToSleep += secsElem.numberLong() * 1000;
            } else if (auto secondsElem = cmdObj["seconds"]) {
                uassert(51154, "'seconds' must be a number.", secondsElem.isNumber());
                msToSleep += secondsElem.numberLong() * 1000;
            }

            if (auto millisElem = cmdObj["millis"]) {
                uassert(34345, "'millis' must be a number.", millisElem.isNumber());
                msToSleep += millisElem.numberLong();
            }
        } else {
            msToSleep = 10 * 1000;
        }

        auto now = opCtx->getServiceContext()->getFastClockSource()->now();
        auto deadline = now + Milliseconds(msToSleep);

        // Note that if the system clock moves _backwards_ (which has been known to happen), this
        // could result in a much longer sleep than requested. Since this command is only used for
        // testing, we're okay with this imprecision.
        while (deadline > now) {
            Milliseconds msRemaining = deadline - now;

            // If the clock moves back by an absurd amount then uassert.
            Milliseconds threshold(10000);
            uassert(31173,
                    str::stream() << "Clock must have moved backwards by at least " << threshold
                                  << " ms during sleep command",
                    msRemaining.count() < msToSleep + threshold.count());

            ON_BLOCK_EXIT(
                [&now, opCtx] { now = opCtx->getServiceContext()->getFastClockSource()->now(); });

            StringData lockTarget;
            if (cmdObj["lockTarget"]) {
                lockTarget = cmdObj["lockTarget"].checkAndGetStringData();
            }

            if (lockTarget == "ParallelBatchWriterMode") {
                _sleepInPBWM(opCtx, msRemaining.count());
                continue;
            }

            if (lockTarget == "RSTL") {
                _sleepInRSTL(opCtx, msRemaining.count());
                continue;
            }

            if (!cmdObj["lock"]) {
                // The caller may specify either 'w' as true or false to take a global X lock or
                // global S lock, respectively.
                if (cmdObj.getBoolField("w")) {
                    _sleepInLock(opCtx, msRemaining.count(), MODE_X, lockTarget);
                } else {
                    _sleepInLock(opCtx, msRemaining.count(), MODE_S, lockTarget);
                }
            } else {
                uassert(34346, "Only one of 'w' and 'lock' may be set.", !cmdObj["w"]);

                std::string lock(cmdObj.getStringField("lock"));
                if (lock == "none") {
                    opCtx->sleepFor(Milliseconds(msRemaining));
                } else if (lock == "w") {
                    _sleepInLock(opCtx, msRemaining.count(), MODE_X, lockTarget);
                } else if (lock == "iw") {
                    _sleepInLock(opCtx, msRemaining.count(), MODE_IX, lockTarget);
                } else if (lock == "r") {
                    _sleepInLock(opCtx, msRemaining.count(), MODE_S, lockTarget);
                } else {
                    uassert(
                        34347, "'lock' must be one of 'r', 'ir', 'w', 'iw', 'none'.", lock == "ir");
                    _sleepInLock(opCtx, msRemaining.count(), MODE_IS, lockTarget);
                }
            }
        }

        // Interrupt point for testing (e.g. maxTimeMS).
        opCtx->checkForInterrupt();

        return true;
    }
};

MONGO_REGISTER_TEST_COMMAND(CmdSleep);
}  // namespace mongo
