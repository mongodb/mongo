// wiredtiger_server_status.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include "mongo/db/storage/wiredtiger/wiredtiger_server_status.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::string;

WiredTigerServerStatusSection::WiredTigerServerStatusSection(WiredTigerKVEngine* engine)
    : ServerStatusSection(kWiredTigerEngineName), _engine(engine) {}

bool WiredTigerServerStatusSection::includeByDefault() const {
    return true;
}

BSONObj WiredTigerServerStatusSection::generateSection(OperationContext* txn,
                                                       const BSONElement& configElement) const {
    // The session does not open a transaction here as one is not needed and opening one would
    // mean that execution could become blocked when a new transaction cannot be allocated
    // immediately.
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(txn)->getSessionNoTxn(txn);
    invariant(session);

    WT_SESSION* s = session->getSession();
    invariant(s);
    const string uri = "statistics:";

    BSONObjBuilder bob;
    Status status = WiredTigerUtil::exportTableToBSON(s, uri, "statistics=(fast)", &bob);
    if (!status.isOK()) {
        bob.append("error", "unable to retrieve statistics");
        bob.append("code", static_cast<int>(status.code()));
        bob.append("reason", status.reason());
    }

    WiredTigerKVEngine::appendGlobalStats(bob);

    return bob.obj();
}

}  // namespace mongo
