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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

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
#include "mongo/db/operation_context.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

    // Until we are able to resolve resource ids to db/collection names, report local db specially
    const ResourceId resourceIdLocalDb = ResourceId(RESOURCE_DATABASE, std::string("local"));

} // namespace


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

        const bool includeAll = q.query["$all"].trueValue();

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

        BSONArrayBuilder inprogBuilder(retVal.subarrayStart("inprog"));

        boost::mutex::scoped_lock scopedLock(Client::clientsMutex);

        ClientSet::const_iterator it = Client::clients.begin();
        for ( ; it != Client::clients.end(); it++) {
            Client* client = *it;
            invariant(client);

            boost::unique_lock<Client> uniqueLock(*client);
            const OperationContext* opCtx = client->getOperationContext();

            if (!includeAll) {
                // Skip over inactive connections.
                if (!opCtx || !opCtx->getCurOp() || !opCtx->getCurOp()->active()) {
                    continue;
                }
            }

            BSONObjBuilder infoBuilder;

            // The client information
            client->reportState(infoBuilder);

            // Operation context specific information
            if (opCtx) {
                // CurOp
                if (opCtx->getCurOp()) {
                    opCtx->getCurOp()->reportState(&infoBuilder);
                }

                // LockState
                Locker::LockerInfo lockerInfo;
                client->getOperationContext()->lockState()->getLockerInfo(&lockerInfo);
                fillLockerInfo(lockerInfo, infoBuilder);
            }

            infoBuilder.done();

            const BSONObj info = infoBuilder.obj();

            if (includeAll || matcher.matches(info)) {
                inprogBuilder.append(info);
            }
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
