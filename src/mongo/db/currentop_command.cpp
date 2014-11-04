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

    void inProgCmd(OperationContext* txn, Message &m, DbResponse &dbresponse) {
        DbMessage d(m);
        QueryMessage q(d);
        BSONObjBuilder b;

        const bool isAuthorized =
                txn->getClient()->getAuthorizationSession()->isAuthorizedForActionsOnResource(
                                                            ResourcePattern::forClusterResource(),
                                                            ActionType::inprog);
        audit::logInProgAuthzCheck(txn->getClient(),
                                   q.query,
                                   isAuthorized ? ErrorCodes::OK : ErrorCodes::Unauthorized);
        if (!isAuthorized) {
            b.append("err", "unauthorized");
            replyToQuery(0, m, dbresponse, b.obj());
            return;
        }

        bool all = q.query["$all"].trueValue();
        vector<BSONObj> vals;
        {
            BSONObj filter;
            {
                BSONObjBuilder b;
                BSONObjIterator i( q.query );
                while ( i.more() ) {
                    BSONElement e = i.next();
                    if ( str::equals( "$all", e.fieldName() ) )
                        continue;
                    b.append( e );
                }
                filter = b.obj();
            }

            const NamespaceString nss(d.getns());

            Client& me = *txn->getClient();
            scoped_lock bl(Client::clientsMutex);
            Matcher m(filter, WhereCallbackReal(txn, nss.db()));
            for( set<Client*>::iterator i = Client::clients.begin(); i != Client::clients.end(); i++ ) {
                Client *c = *i;
                verify( c );
                CurOp* co = c->curop();
                if ( c == &me && !co ) {
                    continue;
                }
                verify( co );
                if( all || co->displayInCurop() ) {
                    BSONObjBuilder infoBuilder;

                    c->reportState(infoBuilder);
                    co->reportState(&infoBuilder);

                    const BSONObj info = infoBuilder.obj();
                    if ( all || m.matches( info )) {
                        vals.push_back( info );
                    }
                }
            }
        }
        b.append("inprog", vals);
        if( lockedForWriting() ) {
            b.append("fsyncLock", true);
            b.append("info", "use db.fsyncUnlock() to terminate the fsync write/snapshot lock");
        }

        replyToQuery(0, m, dbresponse, b.obj());
    }

} // namespace mongo
