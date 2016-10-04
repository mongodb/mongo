// s/request.cpp

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
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/commands/request.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/stats/counters.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/grid.h"
#include "mongo/transport/session.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::string;

Request::Request(Message& m)
    : _clientInfo(&cc()), _m(m), _d(m), _id(_m.header().getId()), _didInit(false) {
    ClusterLastErrorInfo::get(_clientInfo).newRequest();
}

void Request::init(OperationContext* txn) {
    if (_didInit) {
        return;
    }

    _m.header().setId(_id);
    LastError::get(_clientInfo).startRequest();
    ClusterLastErrorInfo::get(_clientInfo).clearRequestInfo();

    if (_d.messageShouldHaveNs()) {
        const NamespaceString nss(getns());

        uassert(ErrorCodes::IllegalOperation,
                "can't use 'local' database through mongos",
                nss.db() != "local");

        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid ns [" << nss.ns() << "]",
                nss.isValid());
    }

    AuthorizationSession::get(_clientInfo)->startRequest(txn);
    _didInit = true;
}

void Request::process(OperationContext* txn, int attempt) {
    init(txn);
    int op = _m.operation();
    verify(op > dbMsg);

    const int32_t msgId = _m.header().getId();

    LOG(3) << "Request::process begin ns: " << getnsIfPresent() << " msg id: " << msgId
           << " op: " << op << " attempt: " << attempt;

    _d.markSet();

    bool iscmd = false;
    if (op == dbKillCursors) {
        Strategy::killCursors(txn, *this);
        globalOpCounters.gotOp(op, iscmd);
    } else if (op == dbQuery) {
        NamespaceString nss(getns());
        iscmd = nss.isCommand() || nss.isSpecialCommand();

        if (iscmd) {
            int n = _d.getQueryNToReturn();
            uassert(16978,
                    str::stream() << "bad numberToReturn (" << n
                                  << ") for $cmd type ns - can only be 1 or -1",
                    n == 1 || n == -1);

            Strategy::clientCommandOp(txn, *this);
        } else {
            Strategy::queryOp(txn, *this);
        }
    } else if (op == dbGetMore) {
        Strategy::getMore(txn, *this);
        globalOpCounters.gotOp(op, iscmd);
    } else {
        Strategy::writeOp(txn, op, *this);
        // globalOpCounters are handled by write commands.
    }

    LOG(3) << "Request::process end ns: " << getnsIfPresent() << " msg id: " << msgId
           << " op: " << op << " attempt: " << attempt;
}

transport::Session* Request::session() const {
    return _clientInfo->session();
}

}  // namespace mongo
