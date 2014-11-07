/**
 *    Copyright (C) 2008-2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommands

#include "mongo/platform/basic.h"

#include "mongo/db/currentop_command.h"

#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {
    // Until we are able to resolve resource ids to db/collection names, report local db specially
    const ResourceId resourceIdLocalDb = ResourceId(RESOURCE_DATABASE, std::string("local"));
}

    /**
     * Populates the BSON array with information about all active clients on the server. A client
     * is active if it has an OperationContext.
     */
    class OperationInfoPopulator : public GlobalEnvironmentExperiment::ProcessOperationContext {
    public:

        OperationInfoPopulator(const Matcher& matcher, BSONArrayBuilder& builder)
            : _matcher(matcher),
              _builder(builder) { 

        }

        virtual void processOpContext(OperationContext* txn) {
            if (!txn->getCurOp() || !txn->getCurOp()->active()) {
                return;
            }

            BSONObjBuilder infoBuilder;

            // Client-specific data
            txn->getClient()->reportState(infoBuilder);

            // Locking data
            Locker::LockerInfo lockerInfo;
            txn->lockState()->getLockerInfo(&lockerInfo);
            fillLockerInfo(lockerInfo, infoBuilder);

            infoBuilder.done();

            const BSONObj info = infoBuilder.obj();

            if (_matcher.matches(info)) {
                _builder.append(info);
            }
        }

    private:
        const Matcher& _matcher;
        BSONArrayBuilder& _builder;
    };


    void inProgCmd(OperationContext* txn, Message &message, DbResponse &dbresponse) {
        DbMessage d(message);
        QueryMessage q(d);

        const bool isAuthorized =
                txn->getClient()->getAuthorizationSession()->isAuthorizedForActionsOnResource(
                                                            ResourcePattern::forClusterResource(),
                                                            ActionType::inprog);
        audit::logInProgAuthzCheck(txn->getClient(),
                                   q.query,
                                   isAuthorized ? ErrorCodes::OK : ErrorCodes::Unauthorized);

        BSONObjBuilder retVal;

        if (!isAuthorized) {
            retVal.append("err", "unauthorized");
            replyToQuery(0, message, dbresponse, retVal.obj());
            return;
        }

        BSONArrayBuilder inprogBuilder(retVal.subarrayStart("inprog"));

        // This lock is acquired for both cases (whether we iterate through the clients list or
        // through the OperationContexts), because we want the CurOp to not be destroyed from
        // underneath and ~CurOp synchronizes on the clients mutex.
        //
        // TODO: This is a legacy from 2.6, which needs to be fixed.
        boost::mutex::scoped_lock scopedLock(Client::clientsMutex);

        const bool all = q.query["$all"].trueValue();
        if (all) {
            for (ClientSet::const_iterator i = Client::clients.begin();
                 i != Client::clients.end();
                 i++) {

                Client* client = *i;
                invariant(client);

                CurOp* curop = client->curop();
                if ((client == txn->getClient()) && !curop) {
                    continue;
                }

                BSONObjBuilder infoBuilder;
                client->reportState(infoBuilder);
                infoBuilder.done();

                inprogBuilder.append(infoBuilder.obj());
            }
        }
        else {

            // Filter the output
            BSONObj filter;
            {
                BSONObjBuilder b;
                BSONObjIterator i(q.query);
                while (i.more()) {
                    BSONElement e = i.next();
                    if (str::equals("$all", e.fieldName())) {
                        continue;
                    }

                    b.append(e);
                }
                filter = b.obj();
            }

            const NamespaceString nss(d.getns());
            const WhereCallbackReal whereCallback(txn, nss.db());
            const Matcher matcher(filter, whereCallback);

            OperationInfoPopulator allOps(matcher, inprogBuilder);
            getGlobalEnvironment()->forEachOperationContext(&allOps);
        }

        inprogBuilder.done();

        if (lockedForWriting()) {
            retVal.append("fsyncLock", true);
            retVal.append("info",
                          "use db.fsyncUnlock() to terminate the fsync write/snapshot lock");
        }

        replyToQuery(0, message, dbresponse, retVal.obj());
    }


    void fillLockerInfo(const Locker::LockerInfo& lockerInfo, BSONObjBuilder& infoBuilder) {
        // "locks" section
        BSONObjBuilder locks(infoBuilder.subobjStart("locks"));
        for (size_t i = 0; i < lockerInfo.locks.size(); i++) {
            const Locker::OneLock& lock = lockerInfo.locks[i];

            if (resourceIdLocalDb == lock.resourceId) {
                locks.append("local", legacyModeName(lock.mode));
            }
            else {
                locks.append(
                    resourceTypeName(lock.resourceId.getType()), legacyModeName(lock.mode));
            }
        }
        locks.done();

        // "waitingForLock" section
        infoBuilder.append("waitingForLock", lockerInfo.waitingResource.isValid());

        // "lockStats" section
        BSONObjBuilder lockStats(infoBuilder.subobjStart("lockStats"));
        // TODO: When we implement lock stats tracking
        lockStats.done();
    }

} // namespace mongo
