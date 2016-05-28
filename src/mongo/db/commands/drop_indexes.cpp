// drop_indexes.cpp

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

#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"

namespace mongo {

using std::endl;
using std::string;
using std::stringstream;
using std::vector;

/* "dropIndexes" is now the preferred form - "deleteIndexes" deprecated */
class CmdDropIndexes : public Command {
public:
    virtual bool slaveOk() const {
        return false;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    virtual void help(stringstream& help) const {
        help << "drop indexes for a collection";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::dropIndex);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    CmdDropIndexes() : Command("dropIndexes", false, "deleteIndexes") {}
    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& jsobj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        const NamespaceString nss = parseNsCollectionRequired(dbname, jsobj);
        return appendCommandStatus(result, dropIndexes(txn, nss, jsobj, &result));
    }

} cmdDropIndexes;

class CmdReIndex : public Command {
public:
    virtual bool slaveOk() const {
        return true;
    }  // can reindex on a secondary
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    virtual void help(stringstream& help) const {
        help << "re-index a collection";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::reIndex);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }
    CmdReIndex() : Command("reIndex") {}

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& jsobj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        DBDirectClient db(txn);

        const NamespaceString toDeleteNs = parseNsCollectionRequired(dbname, jsobj);

        LOG(0) << "CMD: reIndex " << toDeleteNs << endl;

        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dbXLock(txn->lockState(), dbname, MODE_X);
        OldClientContext ctx(txn, toDeleteNs.ns());

        Collection* collection = ctx.db()->getCollection(toDeleteNs.ns());

        if (!collection) {
            errmsg = "ns not found";
            return false;
        }

        BackgroundOperation::assertNoBgOpInProgForNs(toDeleteNs.ns());

        vector<BSONObj> all;
        {
            vector<string> indexNames;
            collection->getCatalogEntry()->getAllIndexes(txn, &indexNames);
            for (size_t i = 0; i < indexNames.size(); i++) {
                const string& name = indexNames[i];
                BSONObj spec = collection->getCatalogEntry()->getIndexSpec(txn, name);
                all.push_back(spec.removeField("v").getOwned());

                const BSONObj key = spec.getObjectField("key");
                const Status keyStatus = validateKeyPattern(key);
                if (!keyStatus.isOK()) {
                    errmsg = str::stream()
                        << "Cannot rebuild index " << spec << ": " << keyStatus.reason()
                        << " For more info see http://dochub.mongodb.org/core/index-validation";
                    return false;
                }
            }
        }

        result.appendNumber("nIndexesWas", all.size());

        {
            WriteUnitOfWork wunit(txn);
            Status s = collection->getIndexCatalog()->dropAllIndexes(txn, true);
            if (!s.isOK()) {
                errmsg = "dropIndexes failed";
                return appendCommandStatus(result, s);
            }
            wunit.commit();
        }

        MultiIndexBlock indexer(txn, collection);
        // do not want interruption as that will leave us without indexes.

        Status status = indexer.init(all);
        if (!status.isOK())
            return appendCommandStatus(result, status);

        status = indexer.insertAllDocumentsInCollection();
        if (!status.isOK())
            return appendCommandStatus(result, status);

        {
            WriteUnitOfWork wunit(txn);
            indexer.commit();
            wunit.commit();
        }

        // Do not allow majority reads from this collection until all original indexes are visible.
        // This was also done when dropAllIndexes() committed, but we need to ensure that no one
        // tries to read in the intermediate state where all indexes are newer than the current
        // snapshot so are unable to be used.
        auto replCoord = repl::ReplicationCoordinator::get(txn);
        auto snapshotName = replCoord->reserveSnapshotName(txn);
        replCoord->forceSnapshotCreation();  // Ensures a newer snapshot gets created even if idle.
        collection->setMinimumVisibleSnapshot(snapshotName);

        result.append("nIndexes", (int)all.size());
        result.append("indexes", all);

        return true;
    }
} cmdReIndex;
}
