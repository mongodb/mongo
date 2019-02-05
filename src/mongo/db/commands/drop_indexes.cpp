// drop_indexes.cpp


/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
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
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"

namespace mongo {

using std::endl;
using std::string;
using std::stringstream;
using std::vector;

/* "dropIndexes" is now the preferred form - "deleteIndexes" deprecated */
class CmdDropIndexes : public BasicCommand {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    std::string help() const override {
        return "drop indexes for a collection";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::dropIndex);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    CmdDropIndexes() : BasicCommand("dropIndexes", "deleteIndexes") {}
    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& jsobj,
             BSONObjBuilder& result) {
        const NamespaceString nss = CommandHelpers::parseNsCollectionRequired(dbname, jsobj);
        uassertStatusOK(dropIndexes(opCtx, nss, jsobj, &result));
        return true;
    }

} cmdDropIndexes;

class CmdReIndex : public ErrmsgCommandDeprecated {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;  // can reindex on a secondary
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    std::string help() const override {
        return "re-index a collection";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::reIndex);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }
    CmdReIndex() : ErrmsgCommandDeprecated("reIndex") {}

    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& jsobj,
                   string& errmsg,
                   BSONObjBuilder& result) {
        DBDirectClient db(opCtx);

        const NamespaceString toReIndexNss =
            CommandHelpers::parseNsCollectionRequired(dbname, jsobj);

        LOG(0) << "CMD: reIndex " << toReIndexNss;

        // This Global write lock is necessary to ensure no other connections establish a snapshot
        // while the reIndex command is running.  The reIndex command does not write oplog entries
        // (for the most part) and thus the minimumVisibleSnapshot mechanism doesn't completely
        // avoid reading at times that may show discrepancies between the in-memory index catalog
        // and the on-disk index catalog.
        Lock::GlobalWrite lk(opCtx);
        AutoGetOrCreateDb autoDb(opCtx, dbname, MODE_X);

        Collection* collection = autoDb.getDb()->getCollection(opCtx, toReIndexNss);
        if (!collection) {
            if (autoDb.getDb()->getViewCatalog()->lookup(opCtx, toReIndexNss.ns()))
                uasserted(ErrorCodes::CommandNotSupportedOnView, "can't re-index a view");
            else
                uasserted(ErrorCodes::NamespaceNotFound, "collection does not exist");
        }

        BackgroundOperation::assertNoBgOpInProgForNs(toReIndexNss.ns());

        // This is necessary to set up CurOp and update the Top stats.
        OldClientContext ctx(opCtx, toReIndexNss.ns());

        const auto defaultIndexVersion = IndexDescriptor::getDefaultIndexVersion();

        vector<BSONObj> all;
        {
            vector<string> indexNames;
            collection->getCatalogEntry()->getAllIndexes(opCtx, &indexNames);
            all.reserve(indexNames.size());

            for (size_t i = 0; i < indexNames.size(); i++) {
                const string& name = indexNames[i];
                BSONObj spec = collection->getCatalogEntry()->getIndexSpec(opCtx, name);

                {
                    BSONObjBuilder bob;

                    for (auto&& indexSpecElem : spec) {
                        auto indexSpecElemFieldName = indexSpecElem.fieldNameStringData();
                        if (IndexDescriptor::kIndexVersionFieldName == indexSpecElemFieldName) {
                            // We create a new index specification with the 'v' field set as
                            // 'defaultIndexVersion'.
                            bob.append(IndexDescriptor::kIndexVersionFieldName,
                                       static_cast<int>(defaultIndexVersion));
                        } else {
                            bob.append(indexSpecElem);
                        }
                    }

                    all.push_back(bob.obj());
                }

                const BSONObj key = spec.getObjectField("key");
                const Status keyStatus =
                    index_key_validate::validateKeyPattern(key, defaultIndexVersion);
                if (!keyStatus.isOK()) {
                    errmsg = str::stream()
                        << "Cannot rebuild index " << spec << ": " << keyStatus.reason()
                        << " For more info see http://dochub.mongodb.org/core/index-validation";
                    return false;
                }
            }
        }

        result.appendNumber("nIndexesWas", all.size());

        std::unique_ptr<MultiIndexBlock> indexer;
        StatusWith<std::vector<BSONObj>> swIndexesToRebuild(ErrorCodes::UnknownError,
                                                            "Uninitialized");

        {
            WriteUnitOfWork wunit(opCtx);
            collection->getIndexCatalog()->dropAllIndexes(opCtx, true);

            indexer = std::make_unique<MultiIndexBlock>(opCtx, collection);

            swIndexesToRebuild = indexer->init(all, MultiIndexBlock::kNoopOnInitFn);
            uassertStatusOK(swIndexesToRebuild.getStatus());
            wunit.commit();
        }

        auto status = indexer->insertAllDocumentsInCollection();
        uassertStatusOK(status);

        {
            WriteUnitOfWork wunit(opCtx);
            uassertStatusOK(indexer->commit(MultiIndexBlock::kNoopOnCreateEachFn,
                                            MultiIndexBlock::kNoopOnCommitFn));
            wunit.commit();
        }

        // Do not allow majority reads from this collection until all original indexes are visible.
        // This was also done when dropAllIndexes() committed, but we need to ensure that no one
        // tries to read in the intermediate state where all indexes are newer than the current
        // snapshot so are unable to be used.
        auto clusterTime = LogicalClock::getClusterTimeForReplicaSet(opCtx).asTimestamp();
        collection->setMinimumVisibleSnapshot(clusterTime);

        result.append("nIndexes", static_cast<int>(swIndexesToRebuild.getValue().size()));
        result.append("indexes", swIndexesToRebuild.getValue());

        return true;
    }
} cmdReIndex;
}
