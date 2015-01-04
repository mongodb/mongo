/**
*    Copyright (C) 2009 10gen Inc.
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

#include "mongo/platform/basic.h"

#include <boost/scoped_ptr.hpp>
#include <boost/thread/locks.hpp>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/currentop_command.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/dbwebserver.h"
#include "mongo/util/mongoutils/html.h"
#include "mongo/util/stringutils.h"

namespace mongo {

    using boost::scoped_ptr;

namespace {

    class ClientListPlugin : public WebStatusPlugin {
    public:
        ClientListPlugin() : WebStatusPlugin( "clients" , 20 ) {}
        virtual void init() {}

        virtual void run(OperationContext* txn, std::stringstream& ss ) {
            using namespace html;

            ss << "\n<table border=1 cellpadding=2 cellspacing=0>";
            ss << "<tr align='left'>"
               << th( a("", "Connections to the database, both internal and external.", "Client") )
               << th( a("http://dochub.mongodb.org/core/viewingandterminatingcurrentoperation", "", "OpId") )
               << "<th>Locking</th>"
               << "<th>Waiting</th>"
               << "<th>SecsRunning</th>"
               << "<th>Op</th>"
               << th( a("http://dochub.mongodb.org/core/whatisanamespace", "", "Namespace") )
               << "<th>Query</th>"
               << "<th>client</th>"
               << "<th>msg</th>"
               << "<th>progress</th>"

               << "</tr>\n";
            
            _processAllClients(ss);
            
            ss << "</table>\n";
        }

    private:

        static void _processAllClients(std::stringstream& ss) {
            using namespace html;

            boost::mutex::scoped_lock scopedLock(Client::clientsMutex);

            ClientSet::const_iterator it = Client::clients.begin();
            for (; it != Client::clients.end(); it++) {
                Client* client = *it;
                invariant(client);

                // Make the client stable
                boost::unique_lock<Client> clientLock(*client);
                const OperationContext* txn = client->getOperationContext();
                if (!txn) continue;

                CurOp* curOp = txn->getCurOp();
                if (!curOp) continue;

                ss << "<tr><td>" << client->desc() << "</td>";

                tablecell(ss, curOp->opNum());
                tablecell(ss, curOp->active());

                // LockState
                {
                    Locker::LockerInfo lockerInfo;
                    txn->lockState()->getLockerInfo(&lockerInfo);

                    BSONObjBuilder lockerInfoBuilder;
                    fillLockerInfo(lockerInfo, lockerInfoBuilder);

                    tablecell(ss, lockerInfoBuilder.obj());
                }

                if (curOp->active()) {
                    tablecell(ss, curOp->elapsedSeconds());
                }
                else {
                    tablecell(ss, "");
                }

                tablecell(ss, curOp->getOp());
                tablecell(ss, html::escape(curOp->getNS()));

                if (curOp->haveQuery()) {
                    tablecell(ss, html::escape(curOp->query().toString()));
                }
                else {
                    tablecell(ss, "");
                }

                tablecell(ss, curOp->getRemoteString());

                tablecell(ss, curOp->getMessage());
                tablecell(ss, curOp->getProgressMeter().toString());

                ss << "</tr>\n";
            }
        }

    } clientListPlugin;


    class CurrentOpContexts : public Command {
    public:
        CurrentOpContexts()
            : Command( "currentOpCtx" ) {
        }

        virtual bool isWriteCommandForConfigServer() const { return false; }

        virtual bool slaveOk() const { return true; }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            if ( client->getAuthorizationSession()
                 ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                    ActionType::inprog) ) {
                return Status::OK();
            }

            return Status(ErrorCodes::Unauthorized, "unauthorized");

        }

        bool run( OperationContext* txn,
                  const string& dbname,
                  BSONObj& cmdObj,
                  int,
                  string& errmsg,
                  BSONObjBuilder& result,
                  bool fromRepl) {

            scoped_ptr<MatchExpression> filter;
            if ( cmdObj["filter"].isABSONObj() ) {
                StatusWithMatchExpression res =
                    MatchExpressionParser::parse( cmdObj["filter"].Obj() );
                if ( !res.isOK() ) {
                    return appendCommandStatus( result, res.getStatus() );
                }
                filter.reset( res.getValue() );
            }

            result.appendArray("operations", _processAllClients(filter.get()));

            return true;
        }


    private:

        static BSONArray _processAllClients(MatchExpression* matcher) {
            BSONArrayBuilder array;

            boost::mutex::scoped_lock scopedLock(Client::clientsMutex);

            ClientSet::const_iterator it = Client::clients.begin();
            for (; it != Client::clients.end(); it++) {
                Client* client = *it;
                invariant(client);

                BSONObjBuilder b;

                // Make the client stable
                boost::unique_lock<Client> clientLock(*client);

                client->reportState(b);

                const OperationContext* txn = client->getOperationContext();
                if (txn) {

                    // CurOp
                    if (txn->getCurOp()) {
                        txn->getCurOp()->reportState(&b);
                    }

                    // LockState
                    if (txn->lockState()) {
                        StringBuilder ss;
                        ss << txn->lockState();
                        b.append("lockStatePointer", ss.str());

                        Locker::LockerInfo lockerInfo;
                        txn->lockState()->getLockerInfo(&lockerInfo);

                        BSONObjBuilder lockerInfoBuilder;
                        fillLockerInfo(lockerInfo, lockerInfoBuilder);

                        b.append("lockState", lockerInfoBuilder.obj());
                    }

                    // RecoveryUnit
                    if (txn->recoveryUnit()) {
                        txn->recoveryUnit()->reportState(&b);
                    }
                }

                const BSONObj obj = b.obj();

                if (!matcher || matcher->matchesBSON(obj)) {
                    array.append(obj);
                }
            }

            return array.arr();
        }

    } currentOpContexts;

}  // namespace
}  // namespace mongo
