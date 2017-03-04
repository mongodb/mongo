// resize_oplog.cpp

/**
*    Copyright (C) 2013 10gen Inc.
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

#include <string>
#include <vector>

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/apply_ops.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/dbhash.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::string;
using std::stringstream;

class CmdResizeOplog : public Command {
public:
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void help(stringstream& help) const {
        help << "resize oplog size, only support wiredTiger";
    }
    CmdResizeOplog() : Command("resizeOplog") {}
    bool run(OperationContext* txn,
            const string& dbname,
            BSONObj& jsobj,
            int,
            string& errmsg,
            BSONObjBuilder& result) {
        StringData dbName("local");
        ScopedTransaction transaction(txn, MODE_IX);
        AutoGetDb autoDb(txn, dbName, MODE_X);
        Database* const db = autoDb.getDb();
        Collection* coll = db ? db->getCollection("local.oplog.rs") : nullptr;
        if (!coll) {
            return appendCommandStatus(result, Status(ErrorCodes::NamespaceNotFound, "ns does not exist"));
        }
        if (!coll->isCapped()) {
            return appendCommandStatus(result, Status(ErrorCodes::InternalError, "ns does not exist"));
        }
        if (!jsobj["size"].isNumber()) {
            return appendCommandStatus(result, Status(ErrorCodes::InvalidOptions, "invalid size field, size should be a number"));
        }

        long long size = jsobj["size"].numberLong();
            WriteUnitOfWork wunit(txn);
        Status status = coll->getRecordStore()->updateCappedSize(txn, size);
        if (!status.isOK()) {
            return appendCommandStatus(result, status);
        }
        CollectionCatalogEntry* entry = coll->getCatalogEntry();
        entry->updateCappedSize(txn, size);
        wunit.commit();
        LOG(1) << "resizeOplog success, currentSize:" << size;
        return appendCommandStatus(result, Status::OK());
    }
} cmdResizeOplog;
}
