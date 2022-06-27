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


#include <string>

#include "mongo/platform/basic.h"

#include "mongo/db/commands/test_commands.h"

#include "mongo/base/init.h"
#include "mongo/db/catalog/capped_utils.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

namespace {
const NamespaceString kDurableHistoryTestNss("mdb_testing.pinned_timestamp");
const std::string kTestingDurableHistoryPinName = "_testing";
}  // namespace

using repl::UnreplicatedWritesBlock;
using std::endl;
using std::string;
using std::stringstream;

/* For testing only, not for general use. Enabled via command-line */
class GodInsert : public ErrmsgCommandDeprecated {
public:
    GodInsert() : ErrmsgCommandDeprecated("godinsert") {}
    virtual bool adminOnly() const {
        return false;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    // No auth needed because it only works when enabled via command line.
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {}
    std::string help() const override {
        return "internal. for testing only.";
    }
    virtual bool errmsgRun(OperationContext* opCtx,
                           const string& dbname,
                           const BSONObj& cmdObj,
                           string& errmsg,
                           BSONObjBuilder& result) {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbname, cmdObj));
        LOGV2(20505,
              "Test-only command 'godinsert' invoked coll:{collection}",
              "Test-only command 'godinsert' invoked",
              "collection"_attr = nss.coll());
        BSONObj obj = cmdObj["obj"].embeddedObjectUserCheck();

        // TODO SERVER-66561 Use DatabaseName obj passed in
        DatabaseName dbName(boost::none, dbname);
        Lock::DBLock lk(opCtx, dbName, MODE_X);
        OldClientContext ctx(opCtx, nss.ns());
        Database* db = ctx.db();

        WriteUnitOfWork wunit(opCtx);
        UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
        CollectionPtr collection =
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        if (!collection) {
            collection = db->createCollection(opCtx, nss);
            if (!collection) {
                errmsg = "could not create collection";
                return false;
            }
        }
        OpDebug* const nullOpDebug = nullptr;
        Status status = collection->insertDocument(opCtx, InsertStatement(obj), nullOpDebug, false);
        if (status.isOK()) {
            wunit.commit();
        }
        uassertStatusOK(status);
        return true;
    }
};

MONGO_REGISTER_TEST_COMMAND(GodInsert);

// Testing only, enabled via command-line.
class CapTrunc : public BasicCommand {
public:
    CapTrunc() : BasicCommand("captrunc") {}
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    // No auth needed because it only works when enabled via command line.
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {}
    virtual bool run(OperationContext* opCtx,
                     const string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        const NamespaceString fullNs = CommandHelpers::parseNsCollectionRequired(dbname, cmdObj);
        if (!fullNs.isValid()) {
            uasserted(ErrorCodes::InvalidNamespace,
                      str::stream() << "collection name " << fullNs.ns() << " is not valid");
        }

        int n = cmdObj.getIntField("n");
        bool inc = cmdObj.getBoolField("inc");  // inclusive range?

        if (n <= 0) {
            uasserted(ErrorCodes::BadValue, "n must be a positive integer");
        }

        // Lock the database in mode IX and lock the collection exclusively.
        AutoGetCollection collection(opCtx, fullNs, MODE_X);
        if (!collection) {
            uasserted(ErrorCodes::NamespaceNotFound,
                      str::stream() << "collection " << fullNs.ns() << " does not exist");
        }

        if (!collection->isCapped()) {
            uasserted(ErrorCodes::IllegalOperation, "collection must be capped");
        }

        RecordId end;
        {
            // Scan backwards through the collection to find the document to start truncating from.
            // We will remove 'n' documents, so start truncating from the (n + 1)th document to the
            // end.
            auto exec = InternalPlanner::collectionScan(opCtx,
                                                        &collection.getCollection(),
                                                        PlanYieldPolicy::YieldPolicy::NO_YIELD,
                                                        InternalPlanner::BACKWARD);

            for (int i = 0; i < n + 1; ++i) {
                PlanExecutor::ExecState state = exec->getNext(static_cast<BSONObj*>(nullptr), &end);
                if (PlanExecutor::ADVANCED != state) {
                    uasserted(ErrorCodes::IllegalOperation,
                              str::stream() << "invalid n, collection contains fewer than " << n
                                            << " documents");
                }
            }
        }

        IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(
            collection->uuid());

        collection->cappedTruncateAfter(opCtx, end, inc);

        return true;
    }
};

MONGO_REGISTER_TEST_COMMAND(CapTrunc);

// Testing-only, enabled via command line.
class EmptyCapped : public BasicCommand {
public:
    EmptyCapped() : BasicCommand("emptycapped") {}
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    // No auth needed because it only works when enabled via command line.
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {}

    virtual bool run(OperationContext* opCtx,
                     const string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        const NamespaceString nss = CommandHelpers::parseNsCollectionRequired(dbname, cmdObj);

        uassertStatusOK(emptyCapped(opCtx, nss));
        return true;
    }
};

MONGO_REGISTER_TEST_COMMAND(EmptyCapped);

class DurableHistoryReplicatedTestCmd : public BasicCommand {
public:
    DurableHistoryReplicatedTestCmd() : BasicCommand("pinHistoryReplicated") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool adminOnly() const override {
        return true;
    }

    bool requiresAuth() const override {
        return false;
    }

    // No auth needed because it only works when enabled via command line.
    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {}

    std::string help() const override {
        return "pins the oldest timestamp";
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const Timestamp requestedPinTs = cmdObj.firstElement().timestamp();
        const bool round = cmdObj["round"].booleanSafe();

        AutoGetDb autoDb(opCtx, kDurableHistoryTestNss.db(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, kDurableHistoryTestNss, MODE_IX);
        if (!CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
                opCtx,
                kDurableHistoryTestNss)) {  // someone else may have beat us to it.
            uassertStatusOK(userAllowedCreateNS(opCtx, kDurableHistoryTestNss));
            WriteUnitOfWork wuow(opCtx);
            CollectionOptions defaultCollectionOptions;
            auto db = autoDb.ensureDbExists(opCtx);
            uassertStatusOK(
                db->userCreateNS(opCtx, kDurableHistoryTestNss, defaultCollectionOptions));
            wuow.commit();
        }

        AutoGetCollection autoColl(opCtx, kDurableHistoryTestNss, MODE_IX);
        WriteUnitOfWork wuow(opCtx);

        // Note, this write will replicate to secondaries, but a secondary will not in-turn pin the
        // oldest timestamp. The write otherwise must be timestamped in a storage engine table with
        // logging disabled. This is to test that rolling back the written document also results in
        // the pin being lifted.
        Timestamp pinTs =
            uassertStatusOK(opCtx->getServiceContext()->getStorageEngine()->pinOldestTimestamp(
                opCtx, kTestingDurableHistoryPinName, requestedPinTs, round));

        uassertStatusOK(autoColl->insertDocument(
            opCtx,
            InsertStatement(fixDocumentForInsert(opCtx, BSON("pinTs" << pinTs)).getValue()),
            nullptr));
        wuow.commit();

        result.append("requestedPinTs", requestedPinTs);
        result.append("pinTs", pinTs);
        return true;
    }
};

MONGO_REGISTER_TEST_COMMAND(DurableHistoryReplicatedTestCmd);

std::string TestingDurableHistoryPin::getName() {
    return kTestingDurableHistoryPinName;
}

boost::optional<Timestamp> TestingDurableHistoryPin::calculatePin(OperationContext* opCtx) {
    AutoGetCollectionForRead autoColl(opCtx, kDurableHistoryTestNss);
    if (!autoColl) {
        return boost::none;
    }

    Timestamp ret = Timestamp::max();
    auto cursor = autoColl->getCursor(opCtx);
    for (auto doc = cursor->next(); doc; doc = cursor->next()) {
        const BSONObj obj = doc.get().data.toBson();
        const Timestamp ts = obj["pinTs"].timestamp();
        ret = std::min(ret, ts);
    }

    if (ret == Timestamp::min()) {
        return boost::none;
    }

    return ret;
}


}  // namespace mongo
