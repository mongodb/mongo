// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_op_observer.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/server_parameter_with_storage.h"
#include "mongo/db/shard_role/ddl/create_gen.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_gen.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_test_gen.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#include <algorithm>
#include <ctime>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <set>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

using namespace cluster_server_parameter_test_util;

const std::vector<NamespaceString> kIgnoredNamespaces = {
    NamespaceString::createNamespaceString_forTest("config"sv, "settings"sv),
    NamespaceString::createNamespaceString_forTest("local"sv, "clusterParameters"sv),
    NamespaceString::createNamespaceString_forTest("test"sv, "foo"sv)};

typedef ClusterParameterWithStorage<ClusterServerParameterTest> ClusterTestParameter;

class ClusterServerParameterOpObserverTest : public ClusterServerParameterTestBase {
public:
    void setUp() override {
        ClusterServerParameterTestBase::setUp();

        auto opCtx = makeOperationContext();
        ASSERT_OK(createCollection(opCtx.get(),
                                   CreateCommand(NamespaceString::kClusterParametersNamespace)));
        for (auto&& nss : kIgnoredNamespaces) {
            ASSERT_OK(createCollection(opCtx.get(), CreateCommand(nss)));
        }
    }

    void switchReplState(const repl::MemberState& newState) {
        auto opCtx = cc().makeOperationContext();
        auto replCoord = repl::ReplicationCoordinator::get(opCtx.get());
        ASSERT_OK(replCoord->setFollowerMode(newState));
    }

    void doInserts(const NamespaceString& nss,
                   std::initializer_list<BSONObj> docs,
                   bool commit = true) {
        std::vector<InsertStatement> stmts;
        std::transform(docs.begin(), docs.end(), std::back_inserter(stmts), [](auto doc) {
            return InsertStatement(doc);
        });
        auto opCtx = cc().makeOperationContext();
        WriteUnitOfWork wuow(opCtx.get());

        AutoGetCollection autoColl(opCtx.get(), nss, MODE_IX);
        observer.onInserts(opCtx.get(),
                           *autoColl,
                           stmts.cbegin(),
                           stmts.cend(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(stmts.size(), false),
                           /*defaultFromMigrate=*/false);
        if (commit)
            wuow.commit();
    }

    void doUpdate(const NamespaceString& nss, BSONObj updatedDoc, bool commit = true) {
        // Actual UUID doesn't matter, just use any...
        const auto criteria = updatedDoc["_id"].wrap();
        const auto preImageDoc = criteria;
        CollectionUpdateArgs updateArgs{preImageDoc};
        updateArgs.criteria = criteria;
        updateArgs.update = BSON("$set" << updatedDoc);
        updateArgs.updatedDoc = updatedDoc;
        auto opCtx = cc().makeOperationContext();
        WriteUnitOfWork wuow(opCtx.get());
        AutoGetCollection autoColl(opCtx.get(), nss, MODE_IX);
        OplogUpdateEntryArgs entryArgs(&updateArgs, *autoColl);
        observer.onUpdate(opCtx.get(), entryArgs);
        if (commit)
            wuow.commit();
    }

    void doDelete(const NamespaceString& nss, BSONObj deletedDoc, bool commit) {
        auto opCtx = cc().makeOperationContext();
        WriteUnitOfWork wuow(opCtx.get());
        AutoGetCollection autoColl(opCtx.get(), nss, MODE_IX);
        const auto& documentKey = getDocumentKey(*autoColl, deletedDoc);
        OplogDeleteEntryArgs args;
        observer.onDelete(opCtx.get(), *autoColl, /*StmtId=*/1, deletedDoc, documentKey, args);
        if (commit)
            wuow.commit();
    }

    void doDropDatabase(const DatabaseName& dbname, bool commit = true) {
        auto opCtx = cc().makeOperationContext();
        WriteUnitOfWork wuow(opCtx.get());
        observer.onDropDatabase(opCtx.get(), dbname, /*markFromMigrate=*/false);
        if (commit)
            wuow.commit();
    }

    void doDropCollection(const NamespaceString& nss, bool commit = true) {
        auto opCtx = cc().makeOperationContext();
        WriteUnitOfWork wuow(opCtx.get());
        AutoGetDb autoDb(opCtx.get(), nss.dbName(), MODE_IX);
        observer.onDropCollection(opCtx.get(),
                                  nss,
                                  UUID::gen(),
                                  0U,
                                  /*markFromMigrate=*/false,
                                  /*isTimeseries=*/false);
        if (commit)
            wuow.commit();
    }


    void doReplicationRollback(const std::vector<NamespaceString>& namespaces) {
        OpObserver::RollbackObserverInfo info;
        for (const auto& nss : namespaces) {
            info.rollbackNamespaces.insert(nss);
        }
        auto opCtx = cc().makeOperationContext();
        observer.onReplicationRollback(opCtx.get(), info);
    }

    // Asserts that the parameter state does not change for this action.
    template <typename F>
    void assertIgnored(const NamespaceString& nss, F fn) {
        auto* sp =
            ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
        ASSERT(sp != nullptr);

        const auto initialCPTime = sp->getClusterParameterTime();
        ClusterServerParameterTest initialCspTest = sp->getValue();
        fn(nss);
        ClusterServerParameterTest finalCspTest = sp->getValue();

        ASSERT_EQ(sp->getClusterParameterTime(), initialCPTime);
        ASSERT_EQ(finalCspTest.getIntValue(), initialCspTest.getIntValue());
        ASSERT_EQ(finalCspTest.getStrValue(), initialCspTest.getStrValue());
    }

    // Asserts that this action causes a failure, and state does not change.
    template <typename F>
    void assertFailure(const NamespaceString& nss, F fn) {
        auto* sp =
            ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
        ASSERT(sp != nullptr);

        const auto initialCPTime = sp->getClusterParameterTime();
        ClusterServerParameterTest initialCspTest = sp->getValue();
        bool failed = false;
        try {
            fn(nss);
        } catch (const DBException&) {
            failed = true;
        }
        ASSERT(failed);
        ClusterServerParameterTest finalCspTest = sp->getValue();

        ASSERT_EQ(sp->getClusterParameterTime(), initialCPTime);
        ASSERT_EQ(finalCspTest.getIntValue(), initialCspTest.getIntValue());
        ASSERT_EQ(finalCspTest.getStrValue(), initialCspTest.getStrValue());
    }

    BSONObj initializeState() {
        Timestamp now(time(nullptr));
        const auto doc =
            makeClusterParametersDoc(LogicalTime(now), kInitialIntValue, kInitialStrValue);

        upsert(doc);
        doInserts(NamespaceString::kClusterParametersNamespace, {doc});

        auto* sp =
            ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
        ASSERT(sp != nullptr);

        ClusterServerParameterTest cspTest = sp->getValue();
        ASSERT_EQ(cspTest.getIntValue(), kInitialIntValue);
        ASSERT_EQ(cspTest.getStrValue(), kInitialStrValue);

        return doc;
    }

    // Asserts that a given action is ignore anywhere outside of cluster parameter collections.
    template <typename F>
    void assertIgnoredOtherNamespaces(F fn) {
        for (const auto& nss : kIgnoredNamespaces) {
            assertIgnored(nss, fn);
        }
    }

    // Asserts that a given action is ignored anywhere, even on cluster parameter collections.
    template <typename F>
    void assertIgnoredAlways(F fn) {
        assertIgnoredOtherNamespaces(fn);
        assertIgnored(NamespaceString::kClusterParametersNamespace, fn);
    }

    template <typename F>
    void assertFailsOnlyCPNamespace(F fn) {
        assertIgnoredOtherNamespaces(fn);
        assertFailure(NamespaceString::kClusterParametersNamespace, fn);
    }

    void assertParameterState(int line,
                              int intVal,
                              std::string_view strVal,
                              boost::optional<LogicalTime> cpt = boost::none) {
        auto* sp =
            ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
        try {
            if (cpt) {
                ASSERT_EQ(sp->getClusterParameterTime(), *cpt);
            }

            ClusterServerParameterTest cspTest = sp->getValue();
            ASSERT_EQ(cspTest.getIntValue(), intVal);
            ASSERT_EQ(cspTest.getStrValue(), strVal);
        } catch (...) {
            LOGV2_ERROR(6887700, "ASSERT_PARAMETER_STATE failed", "line"_attr = line);
            throw;
        }
    }

protected:
    ClusterServerParameterOpObserver observer;
};

#define ASSERT_PARAMETER_STATE(...) assertParameterState(__LINE__, __VA_ARGS__)

TEST_F(ClusterServerParameterOpObserverTest, OnInsertRecord) {
    initializeState();
    auto* sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);

    // Single record insert.
    const auto initialLogicalTime = sp->getClusterParameterTime();
    const auto singleLogicalTime = initialLogicalTime.addTicks(1);
    const auto singleIntValue = sp->getValue().getIntValue() + 1;
    const auto singleStrValue = "OnInsertRecord.single";

    ASSERT_LT(initialLogicalTime, singleLogicalTime);

    // Simulate initial sync and attempt to insert a record.
    switchReplState(repl::MemberState::RS_STARTUP2);
    doInserts(NamespaceString::kClusterParametersNamespace,
              {makeClusterParametersDoc(singleLogicalTime, singleIntValue, singleStrValue)});
    // Ensure that the in-memory state does not change when data is not stable.
    ASSERT_PARAMETER_STATE(kInitialIntValue, kInitialStrValue);

    // Make sure that we are primary.
    switchReplState(repl::MemberState::RS_PRIMARY);

    doInserts(NamespaceString::kClusterParametersNamespace,
              {makeClusterParametersDoc(singleLogicalTime, singleIntValue, singleStrValue)});

    ASSERT_PARAMETER_STATE(singleIntValue, singleStrValue, singleLogicalTime);

    // Multi-record insert.
    const auto multiLogicalTime = singleLogicalTime.addTicks(1);
    const auto multiIntValue = singleIntValue + 1;
    const auto multiStrValue = "OnInsertRecord.multi";

    ASSERT_LT(singleLogicalTime, multiLogicalTime);
    doInserts(
        NamespaceString::kClusterParametersNamespace,
        {
            makeClusterParametersDoc(multiLogicalTime, multiIntValue, multiStrValue, "cspTest2"),
            makeClusterParametersDoc(multiLogicalTime, multiIntValue, multiStrValue),
        });

    ASSERT_PARAMETER_STATE(multiIntValue, multiStrValue, multiLogicalTime);

    // Insert plausible records to namespaces we don't care about.
    assertIgnoredOtherNamespaces([this](const auto& nss) {
        doInserts(nss, {makeClusterParametersDoc(LogicalTime(), 42, "yellow")});
    });
    // Plausible on other NS, multi-insert.
    assertIgnoredOtherNamespaces([this](const auto& nss) {
        auto d0 = makeClusterParametersDoc(LogicalTime(), 123, "red");
        auto d1 = makeClusterParametersDoc(LogicalTime(), 234, "green");
        auto d2 = makeClusterParametersDoc(LogicalTime(), 345, "blue");
        doInserts(nss, {d0, d1, d2});
    });

    // Unknown CSP record fails
    assertFailsOnlyCPNamespace(
        [this](const auto& nss) { doInserts(nss, {BSON("_id" << "ignored")}); });

    // Unknown CSP and not unknown CSP fails, multi-insert.
    assertFailsOnlyCPNamespace([this](const auto& nss) {
        doInserts(
            nss,
            {makeClusterParametersDoc(LogicalTime(), 456, "yellow"), BSON("_id" << "ignored")});
    });
}

TEST_F(ClusterServerParameterOpObserverTest, OnUpdateRecord) {
    initializeState();
    auto* sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);

    // Single record update.
    const auto initialLogicalTime = sp->getClusterParameterTime();
    const auto singleLogicalTime = initialLogicalTime.addTicks(1);
    const auto singleIntValue = sp->getValue().getIntValue() + 1;
    const auto singleStrValue = "OnUpdateRecord.single";
    ASSERT_LT(initialLogicalTime, singleLogicalTime);

    // Simulate initial sync and attempt to update the record.
    switchReplState(repl::MemberState::RS_STARTUP2);
    doUpdate(NamespaceString::kClusterParametersNamespace,
             makeClusterParametersDoc(singleLogicalTime, singleIntValue, singleStrValue));
    // Ensure that the in-memory state does not change when data is not stable.
    ASSERT_PARAMETER_STATE(kInitialIntValue, kInitialStrValue);

    // Make sure that we are primary.
    switchReplState(repl::MemberState::RS_PRIMARY);

    // Perform the record update.
    doUpdate(NamespaceString::kClusterParametersNamespace,
             makeClusterParametersDoc(singleLogicalTime, singleIntValue, singleStrValue));

    ASSERT_PARAMETER_STATE(singleIntValue, singleStrValue, singleLogicalTime);

    // Plausible doc in wrong namespace.
    assertIgnoredOtherNamespaces([this](const auto& nss) {
        doUpdate(nss, makeClusterParametersDoc(LogicalTime(), 123, "ignored"));
    });

    // Non cluster parameter doc fails.
    assertFailsOnlyCPNamespace([this](const auto& nss) {
        doUpdate(nss, BSON(ClusterServerParameter::k_idFieldName << "ignored"));
    });
}

TEST_F(ClusterServerParameterOpObserverTest, onDeleteRecord) {
    auto* sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);

    const auto initialDoc = initializeState();

    // Ignore deletes in other namespaces, whether with or without deleted doc.
    assertIgnoredOtherNamespaces(
        [this, initialDoc](const auto& nss) { doDelete(nss, initialDoc, /*commit=*/true); });

    // Ignore deletes where the _id does not correspond to a known cluster server parameter.
    assertIgnoredAlways([this](const auto& nss) {
        doDelete(nss, BSON(ClusterServerParameter::k_idFieldName << "ignored"), /*commit=*/true);
    });

    // Simulate initial sync and attempt to delete.
    switchReplState(repl::MemberState::RS_STARTUP2);
    doDelete(NamespaceString::kClusterParametersNamespace, initialDoc, /*commit=*/true);
    // Ensure that the in-memory state does not change when data is not stable.
    ASSERT_PARAMETER_STATE(kInitialIntValue, kInitialStrValue);

    // Make sure that we are primary.
    switchReplState(repl::MemberState::RS_PRIMARY);

    // Reset configuration to defaults when we claim to have deleted the doc.
    doDelete(NamespaceString::kClusterParametersNamespace, initialDoc, /*commit=*/true);

    ASSERT_PARAMETER_STATE(kDefaultIntValue, kDefaultStrValue);

    // Restore configured state, and delete.
    initializeState();
    doDelete(NamespaceString::kClusterParametersNamespace, initialDoc, /*commit=*/true);

    ASSERT_PARAMETER_STATE(kDefaultIntValue, kDefaultStrValue);
}

TEST_F(ClusterServerParameterOpObserverTest, onDropDatabase) {
    initializeState();

    // Drop ignorable databases.
    assertIgnoredOtherNamespaces([this](const auto& nss) {
        const auto dbname = nss.dbName();
        if (!dbname.isConfigDB()) {
            doDropDatabase(dbname);
        }
    });

    // Simulate initial sync and attempt to drop the config DB.
    switchReplState(repl::MemberState::RS_STARTUP2);
    doDropDatabase(DatabaseName::kConfig);
    // Ensure that the in-memory state does not change when data is not stable.
    ASSERT_PARAMETER_STATE(kInitialIntValue, kInitialStrValue);

    // Make sure that we are primary.
    switchReplState(repl::MemberState::RS_PRIMARY);

    // Actually drop the config DB.
    doDropDatabase(DatabaseName::kConfig);

    auto* sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);

    ASSERT_PARAMETER_STATE(kDefaultIntValue, kDefaultStrValue);
}

TEST_F(ClusterServerParameterOpObserverTest, onDropCollection) {
    initializeState();

    // Ignore collections dropped in other namespaces
    assertIgnoredOtherNamespaces([this](const auto& nss) { doDropCollection(nss); });

    // Simulate initial sync and attempt to drop the clusterParameters collection.
    switchReplState(repl::MemberState::RS_STARTUP2);
    doDropCollection(NamespaceString::kClusterParametersNamespace);
    // Ensure that the in-memory state does not change when data is not stable.
    ASSERT_PARAMETER_STATE(kInitialIntValue, kInitialStrValue);

    // Make sure that we are primary.
    switchReplState(repl::MemberState::RS_PRIMARY);

    // Actually drop the clusterParameters collection.
    doDropCollection(NamespaceString::kClusterParametersNamespace);

    auto* sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);

    ASSERT_PARAMETER_STATE(kDefaultIntValue, kDefaultStrValue);
}

TEST_F(ClusterServerParameterOpObserverTest, onDropDatabaseRejectsTenantScopedConfigDb) {
    initializeState();

    const auto tenantConfigDb =
        DatabaseName::createDatabaseName_forTest(TenantId(OID::gen()), "config"sv);
    ASSERT_THROWS_CODE(doDropDatabase(tenantConfigDb), DBException, ErrorCodes::IllegalOperation);

    // The global parameter must be untouched.
    ASSERT_PARAMETER_STATE(kInitialIntValue, kInitialStrValue);
}

TEST_F(ClusterServerParameterOpObserverTest, onDropCollectionRejectsTenantScopedNamespace) {
    initializeState();

    const auto tenantNss = NamespaceString::makeClusterParametersNSS(TenantId(OID::gen()));
    ASSERT_THROWS_CODE(doDropCollection(tenantNss), DBException, ErrorCodes::IllegalOperation);

    // The global parameter must be untouched.
    ASSERT_PARAMETER_STATE(kInitialIntValue, kInitialStrValue);
}

TEST_F(ClusterServerParameterOpObserverTest, WritesRejectTenantScopedNamespace) {
    const auto initialDoc = initializeState();

    // A tenant-scoped collection can only be created when multitenancy is enabled.
    const auto oldMultitenancy = std::exchange(gMultitenancySupport, true);
    ScopeGuard restore([&] { gMultitenancySupport = oldMultitenancy; });

    const auto tenantNss = NamespaceString::makeClusterParametersNSS(TenantId(OID::gen()));
    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(createCollection(opCtx.get(), CreateCommand(tenantNss)));
    }

    const auto doc = BSON(ClusterServerParameter::k_idFieldName << kCSPTest);
    ASSERT_THROWS_CODE(doInserts(tenantNss, {doc}), DBException, ErrorCodes::IllegalOperation);
    ASSERT_THROWS_CODE(doUpdate(tenantNss, doc), DBException, ErrorCodes::IllegalOperation);
    ASSERT_THROWS_CODE(
        doDelete(tenantNss, doc, /*commit=*/true), DBException, ErrorCodes::IllegalOperation);

    // The global parameter must be untouched.
    ASSERT_PARAMETER_STATE(kInitialIntValue, kInitialStrValue);
}

TEST_F(ClusterServerParameterOpObserverTest, onReplicationRollback) {
    initializeState();

    const NamespaceString kTestFoo = NamespaceString::createNamespaceString_forTest("test", "foo");
    // Import ignorable collections.
    assertIgnoredOtherNamespaces([&](const auto& nss) { doReplicationRollback({nss}); });

    auto* sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);

    remove();
    // Trigger rollback of ignorable namespaces and ensure no disk reload occurs.
    doReplicationRollback(kIgnoredNamespaces);
    ASSERT_PARAMETER_STATE(kInitialIntValue, kInitialStrValue);

    // Trigger rollback of the cluster parameters namespace and ensure disk reload occurs.
    doReplicationRollback({NamespaceString::kClusterParametersNamespace});

    ASSERT_PARAMETER_STATE(kDefaultIntValue, kDefaultStrValue);
}

TEST_F(ClusterServerParameterOpObserverTest, abortsAfterObservation) {

    const auto initialDoc = initializeState();

    doInserts(NamespaceString::kClusterParametersNamespace,
              {makeClusterParametersDoc(LogicalTime(Timestamp(12345678)), 123, "abc")},
              /*commit=*/false);

    ASSERT_PARAMETER_STATE(kInitialIntValue, kInitialStrValue);

    doUpdate(NamespaceString::kClusterParametersNamespace,
             {makeClusterParametersDoc(LogicalTime(Timestamp(87654321)), 321, "cba")},
             /*commit=*/false);

    ASSERT_PARAMETER_STATE(kInitialIntValue, kInitialStrValue);

    doDelete(NamespaceString::kClusterParametersNamespace, initialDoc, /*commit=*/false);

    ASSERT_PARAMETER_STATE(kInitialIntValue, kInitialStrValue);

    doDropDatabase(DatabaseName::createDatabaseName_forTest(boost::none, kConfigDB),
                   /*commit=*/false);

    ASSERT_PARAMETER_STATE(kInitialIntValue, kInitialStrValue);
}

#undef ASSERT_PARAMETER_STATE

}  // namespace
}  // namespace mongo
