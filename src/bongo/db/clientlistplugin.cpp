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

#include "bongo/platform/basic.h"

#include "bongo/bson/bsonobjbuilder.h"
#include "bongo/db/auth/action_type.h"
#include "bongo/db/auth/authorization_session.h"
#include "bongo/db/client.h"
#include "bongo/db/commands.h"
#include "bongo/db/curop.h"
#include "bongo/db/dbwebserver.h"
#include "bongo/db/matcher/expression_parser.h"
#include "bongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "bongo/db/operation_context.h"
#include "bongo/db/service_context.h"
#include "bongo/db/stats/fill_locker_info.h"
#include "bongo/rpc/metadata/client_metadata.h"
#include "bongo/rpc/metadata/client_metadata_ismaster.h"
#include "bongo/util/bongoutils/html.h"
#include "bongo/util/stringutils.h"

namespace bongo {

using std::unique_ptr;
using std::string;

namespace {

class ClientListPlugin : public WebStatusPlugin {
public:
    ClientListPlugin() : WebStatusPlugin("clients", 20) {}
    virtual void init() {}

    virtual void run(OperationContext* txn, std::stringstream& ss) {
        using namespace html;

        ss << "\n<table border=1 cellpadding=2 cellspacing=0>";
        ss << "<tr align='left'>"
           << th(a("", "Connections to the database, both internal and external.", "Client"))
           << th(a("http://dochub.bongodb.org/core/viewingandterminatingcurrentoperation",
                   "",
                   "OpId"))
           << "<th>Locking</th>"
           << "<th>Waiting</th>"
           << "<th>SecsRunning</th>"
           << "<th>Op</th>"
           << th(a("http://dochub.bongodb.org/core/whatisanamespace", "", "Namespace"))
           << "<th>Query</th>"
           << "<th>client</th>"
           << "<th>msg</th>"
           << "<th>progress</th>"

           << "</tr>\n";

        _processAllClients(txn->getClient()->getServiceContext(), ss);

        ss << "</table>\n";
    }

private:
    static void _processAllClients(ServiceContext* service, std::stringstream& ss) {
        using namespace html;

        for (ServiceContext::LockedClientsCursor cursor(service); Client* client = cursor.next();) {
            invariant(client);

            // Make the client stable
            stdx::lock_guard<Client> lk(*client);
            const OperationContext* txn = client->getOperationContext();
            if (!txn)
                continue;

            CurOp* curOp = CurOp::get(txn);
            if (!curOp)
                continue;

            ss << "<tr><td>" << client->desc() << "</td>";

            tablecell(ss, txn->getOpID());
            tablecell(ss, true);

            // LockState
            {
                Locker::LockerInfo lockerInfo;
                txn->lockState()->getLockerInfo(&lockerInfo);

                BSONObjBuilder lockerInfoBuilder;
                fillLockerInfo(lockerInfo, lockerInfoBuilder);

                tablecell(ss, lockerInfoBuilder.obj());
            }

            tablecell(ss, curOp->elapsedSeconds());

            tablecell(ss, curOp->getNetworkOp());
            tablecell(ss, html::escape(curOp->getNS()));

            if (curOp->haveQuery()) {
                tablecell(ss, html::escape(curOp->query().toString()));
            } else {
                tablecell(ss, "");
            }

            tablecell(ss, client->clientAddress(true /*includePort*/));

            tablecell(ss, curOp->getMessage());
            tablecell(ss, curOp->getProgressMeter().toString());

            ss << "</tr>\n";
        }
    }

} clientListPlugin;

}  // namespace
}  // namespace bongo
