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

#include "mongo/db/commands/test_commands.h"

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <memory>
#include <ostream>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/capped_collection_maintenance.h"
#include "mongo/db/catalog/capped_utils.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/sysprofile_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#ifdef __linux__
#include "mongo/util/sysprofile.h"
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

const std::string kTestingDurableHistoryPinName = "_testing";

using repl::UnreplicatedWritesBlock;
using std::endl;
using std::string;
using std::stringstream;

/**
 * The commands in this file are for testing only, not for general use.
 * For more on this topic and how to enable these commands, see docs/test_commands.md.
 */

class GodInsert : public BasicCommand {
public:
    GodInsert() : BasicCommand("godinsert") {}
    bool adminOnly() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    // No auth needed because it only works when enabled via command line.
    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    std::string help() const override {
        return "internal. for testing only.";
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));
        LOGV2(20505, "Test-only command 'godinsert' invoked", "collection"_attr = nss.coll());
        BSONObj obj = cmdObj["obj"].embeddedObjectUserCheck();

        Lock::DBLock lk(opCtx, dbName, MODE_X);
        OldClientContext ctx(opCtx, nss);
        Database* db = ctx.db();

        auto collection = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);

        WriteUnitOfWork wunit(opCtx);
        UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
        if (!collection.exists()) {
            ScopedLocalCatalogWriteFence scopedLocalCatalogWriteFence(opCtx, &collection);
            db->createCollection(opCtx, nss);
        }
        uassert(
            ErrorCodes::CannotCreateCollection, "could not create collection", collection.exists());

        Status status = Helpers::insert(opCtx, collection, obj);
        if (status.isOK()) {
            wunit.commit();
        }
        uassertStatusOK(status);
        return true;
    }
};

MONGO_REGISTER_COMMAND(GodInsert).testOnly().forShard();

class CapTrunc : public BasicCommand {
public:
    CapTrunc() : BasicCommand("captrunc") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    // No auth needed because it only works when enabled via command line.
    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString fullNs = CommandHelpers::parseNsCollectionRequired(dbName, cmdObj);
        if (!fullNs.isValid()) {
            uasserted(ErrorCodes::InvalidNamespace,
                      str::stream()
                          << "collection name " << fullNs.toStringForErrorMsg() << " is not valid");
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
                      str::stream()
                          << "collection " << fullNs.toStringForErrorMsg() << " does not exist");
        }

        if (!collection->isCapped()) {
            uasserted(ErrorCodes::IllegalOperation, "collection must be capped");
        }

        RecordId end;
        {
            // Scan backwards through the collection to find the document to start truncating
            // from. We will remove 'n' documents, so start truncating from the (n + 1)th
            // document to the end.
            auto exec =
                InternalPlanner::collectionScan(opCtx,
                                                &collection.getCollection(),
                                                PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
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

        collection_internal::cappedTruncateAfter(opCtx, *collection, end, inc);
        return true;
    }
};

MONGO_REGISTER_COMMAND(CapTrunc).testOnly().forShard();

class EmptyCapped : public BasicCommand {
public:
    EmptyCapped() : BasicCommand("emptycapped") {}
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    // No auth needed because it only works when enabled via command line.
    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss = CommandHelpers::parseNsCollectionRequired(dbName, cmdObj);

        uassertStatusOK(emptyCapped(opCtx, nss));
        return true;
    }
};

MONGO_REGISTER_COMMAND(EmptyCapped).testOnly().forShard();

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
    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    std::string help() const override {
        return "pins the oldest timestamp";
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const Timestamp requestedPinTs = cmdObj.firstElement().timestamp();
        const bool round = cmdObj["round"].booleanSafe();

        AutoGetDb autoDb(opCtx, NamespaceString::kDurableHistoryTestNamespace.dbName(), MODE_IX);
        Lock::CollectionLock collLock(
            opCtx, NamespaceString::kDurableHistoryTestNamespace, MODE_IX);
        if (!CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
                opCtx,
                NamespaceString::kDurableHistoryTestNamespace)) {  // someone else may have beat us
                                                                   // to it.
            uassertStatusOK(
                userAllowedCreateNS(opCtx, NamespaceString::kDurableHistoryTestNamespace));
            WriteUnitOfWork wuow(opCtx);
            CollectionOptions defaultCollectionOptions;
            auto db = autoDb.ensureDbExists(opCtx);
            uassertStatusOK(db->userCreateNS(
                opCtx, NamespaceString::kDurableHistoryTestNamespace, defaultCollectionOptions));
            wuow.commit();
        }

        const auto collection = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(
                opCtx,
                static_cast<const NamespaceString&>(NamespaceString::kDurableHistoryTestNamespace),
                AcquisitionPrerequisites::kWrite),
            MODE_IX);
        WriteUnitOfWork wuow(opCtx);

        // Note, this write will replicate to secondaries, but a secondary will not in-turn pin
        // the oldest timestamp. The write otherwise must be timestamped in a storage engine
        // table with logging disabled. This is to test that rolling back the written document
        // also results in the pin being lifted.
        Timestamp pinTs =
            uassertStatusOK(opCtx->getServiceContext()->getStorageEngine()->pinOldestTimestamp(
                opCtx, kTestingDurableHistoryPinName, requestedPinTs, round));

        uassertStatusOK(Helpers::insert(
            opCtx, collection, fixDocumentForInsert(opCtx, BSON("pinTs" << pinTs)).getValue()));
        wuow.commit();

        result.append("requestedPinTs", requestedPinTs);
        result.append("pinTs", pinTs);
        return true;
    }
};

MONGO_REGISTER_COMMAND(DurableHistoryReplicatedTestCmd).testOnly().forShard();

// TODO SERVER-80003 remove this test command when 8.0 branches off.
class TimeseriesCatalogBucketParamsChangedTestCmd : public BasicCommand {
public:
    TimeseriesCatalogBucketParamsChangedTestCmd()
        : BasicCommand("timeseriesCatalogBucketParamsChanged") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    bool requiresAuth() const override {
        return false;
    }

    // No auth needed because it only works when enabled via command line.
    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    std::string help() const override {
        return "return the value of timeseriesCatalogBucketParamsChanged";
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString fullNs = CommandHelpers::parseNsCollectionRequired(dbName, cmdObj);
        AutoGetCollection autoColl(opCtx, fullNs, MODE_IS);
        uassert(7927100, "Could not find a collection with the requested namespace", autoColl);
        auto output = autoColl->timeseriesBucketingParametersHaveChanged();
        if (output) {
            result.append("changed", *output);
        }
        return true;
    }
};

MONGO_REGISTER_COMMAND(TimeseriesCatalogBucketParamsChangedTestCmd).testOnly().forShard();

class SysProfile : public TypedCommand<SysProfile> {
    using _TypedCommandInvocationBase = typename TypedCommand<SysProfile>::InvocationBase;

public:
    using Request = SysProfileCommandRequest;
    using Reply = SysProfileCommandRequest::Reply;

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "Internal profiling command, for testing only. See "
               "https://wiki.corp.mongodb.com/display/~zixuan.zhuang@mongodb.com/"
               "Scripting+Profiler for prerequisite and examples.";
    }

    class Invocation : public _TypedCommandInvocationBase {
    public:
        using _TypedCommandInvocationBase::_TypedCommandInvocationBase;

        bool supportsWriteConcern() const final {
            return false;
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {}

        Reply typedRun(OperationContext* opCtx) {
#ifdef __linux__
            LOGV2(8387208, "Test-only command 'sysprofile' invoked");
            Reply reply(1.0);
            if (auto pid = request().getPid()) {
                // kill profiler
                reply.setOk(sysprofile::stop(*pid));
            } else {
                StringData filename = request().getFilename();
                sysprofile::PerfMode mode = request().getMode() == ProfileModeEnum::record
                    ? sysprofile::PerfMode::record
                    : sysprofile::PerfMode::counters;
                reply.setPid(sysprofile::spawn(filename, mode));
            }
            return reply;
#else
            tasserted(8387207, "Unsupported OS for sysprofile command");
#endif
        }
    };
};

MONGO_REGISTER_COMMAND(SysProfile).testOnly().forShard();
}  // namespace

std::string TestingDurableHistoryPin::getName() {
    return kTestingDurableHistoryPinName;
}

boost::optional<Timestamp> TestingDurableHistoryPin::calculatePin(OperationContext* opCtx) {
    AutoGetCollectionForRead autoColl(opCtx, NamespaceString::kDurableHistoryTestNamespace);
    if (!autoColl) {
        return boost::none;
    }

    Timestamp ret = Timestamp::max();
    auto cursor = autoColl->getCursor(opCtx);
    for (auto doc = cursor->next(); doc; doc = cursor->next()) {
        const BSONObj obj = doc.value().data.toBson();
        const Timestamp ts = obj["pinTs"].timestamp();
        ret = std::min(ret, ts);
    }

    if (ret == Timestamp::min()) {
        return boost::none;
    }

    return ret;
}

}  // namespace mongo
