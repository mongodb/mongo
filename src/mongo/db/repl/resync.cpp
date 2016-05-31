/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/master_slave.h"  // replSettings
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_impl.h"

namespace mongo {

using std::string;
using std::stringstream;

namespace repl {

// operator requested resynchronization of replication (on a slave or secondary). {resync: 1}
class CmdResync : public Command {
public:
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::resync);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    void help(stringstream& h) const {
        h << "resync (from scratch) a stale slave or replica set secondary node.\n";
    }

    CmdResync() : Command("resync") {}
    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        ScopedTransaction transaction(txn, MODE_X);
        Lock::GlobalWrite globalWriteLock(txn->lockState());

        ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();
        if (getGlobalReplicationCoordinator()->getSettings().usingReplSets()) {
            const MemberState memberState = replCoord->getMemberState();
            if (memberState.startup()) {
                return appendCommandStatus(
                    result, Status(ErrorCodes::NotYetInitialized, "no replication yet active"));
            }
            if (memberState.primary() || !replCoord->setFollowerMode(MemberState::RS_STARTUP2)) {
                return appendCommandStatus(
                    result, Status(ErrorCodes::NotSecondary, "primaries cannot resync"));
            }
            replCoord->setInitialSyncRequestedFlag(true);
            return true;
        }

        // below this comment pertains only to master/slave replication
        if (cmdObj.getBoolField("force")) {
            if (!waitForSyncToFinish(txn, errmsg))
                return false;
            replAllDead = "resync forced";
        }
        // TODO(dannenberg) replAllDead is bad and should be removed when masterslave is removed
        if (!replAllDead) {
            errmsg = "not dead, no need to resync";
            return false;
        }
        if (!waitForSyncToFinish(txn, errmsg))
            return false;

        ReplSource::forceResyncDead(txn, "client");
        result.append("info", "triggered resync for all sources");

        return true;
    }

    bool waitForSyncToFinish(OperationContext* txn, string& errmsg) const {
        // Wait for slave thread to finish syncing, so sources will be be
        // reloaded with new saved state on next pass.
        Timer t;
        while (1) {
            if (syncing == 0 || t.millis() > 30000)
                break;
            {
                Lock::TempRelease t(txn->lockState());
                relinquishSyncingSome = 1;
                sleepmillis(1);
            }
        }
        if (syncing) {
            errmsg = "timeout waiting for sync() to finish";
            return false;
        }
        return true;
    }
} cmdResync;
}  // namespace repl
}  // namespace mongo
