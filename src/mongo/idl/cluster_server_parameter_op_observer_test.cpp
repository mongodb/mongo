/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/idl/cluster_server_parameter_op_observer.h"
#include "mongo/idl/cluster_server_parameter_test_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace {

using namespace cluster_server_parameter_test_util;

const std::vector<NamespaceString> kIgnoredNamespaces = {
    NamespaceString::createNamespaceString_forTest("config"_sd, "settings"_sd),
    NamespaceString::createNamespaceString_forTest("local"_sd, "clusterParameters"_sd),
    NamespaceString::createNamespaceString_forTest("test"_sd, "foo"_sd)};

typedef ClusterParameterWithStorage<ClusterServerParameterTest> ClusterTestParameter;

class ClusterServerParameterOpObserverTest : public ClusterServerParameterTestBase {
public:
    void setUp() override {
        ClusterServerParameterTestBase::setUp();

        auto opCtx = makeOperationContext();
        ASSERT_OK(createCollection(opCtx.get(),
                                   CreateCommand(NamespaceString::kClusterParametersNamespace)));
        ASSERT_OK(createCollection(
            opCtx.get(), CreateCommand(NamespaceString::makeClusterParametersNSS(kTenantId))));
        for (auto&& nss : kIgnoredNamespaces) {
            ASSERT_OK(createCollection(opCtx.get(), CreateCommand(nss)));
        }
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

    void doDelete(const NamespaceString& nss,
                  BSONObj deletedDoc,
                  bool includeDeletedDoc = true,
                  bool commit = true) {
        auto opCtx = cc().makeOperationContext();
        WriteUnitOfWork wuow(opCtx.get());
        AutoGetCollection autoColl(opCtx.get(), nss, MODE_IX);
        observer.aboutToDelete(opCtx.get(), *autoColl, deletedDoc);
        OplogDeleteEntryArgs args;
        args.deletedDoc = includeDeletedDoc ? &deletedDoc : nullptr;
        observer.onDelete(opCtx.get(), *autoColl, 1 /* StmtId */, args);
        if (commit)
            wuow.commit();
    }

    void doDropDatabase(const DatabaseName& dbname, bool commit = true) {
        auto opCtx = cc().makeOperationContext();
        WriteUnitOfWork wuow(opCtx.get());
        observer.onDropDatabase(opCtx.get(), dbname);
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
    void assertIgnored(const NamespaceString& nss,
                       F fn,
                       const boost::optional<TenantId>& tenantId) {
        auto* sp =
            ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
        ASSERT(sp != nullptr);

        const auto initialCPTime = sp->getClusterParameterTime(tenantId);
        ClusterServerParameterTest initialCspTest = sp->getValue(tenantId);
        fn(nss);
        ClusterServerParameterTest finalCspTest = sp->getValue(tenantId);

        ASSERT_EQ(sp->getClusterParameterTime(tenantId), initialCPTime);
        ASSERT_EQ(finalCspTest.getIntValue(), initialCspTest.getIntValue());
        ASSERT_EQ(finalCspTest.getStrValue(), initialCspTest.getStrValue());
    }

    std::pair<BSONObj, BSONObj> initializeState() {
        Timestamp now(time(nullptr));
        const auto doc =
            makeClusterParametersDoc(LogicalTime(now), kInitialIntValue, kInitialStrValue);

        upsert(doc, boost::none);
        doInserts(NamespaceString::makeClusterParametersNSS(boost::none), {doc});

        const auto docT = makeClusterParametersDoc(
            LogicalTime(now), kInitialTenantIntValue, kInitialTenantStrValue);

        upsert(docT, kTenantId);
        doInserts(NamespaceString::makeClusterParametersNSS(kTenantId), {docT});

        auto* sp =
            ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
        ASSERT(sp != nullptr);

        ClusterServerParameterTest cspTest = sp->getValue(boost::none);
        ASSERT_EQ(cspTest.getIntValue(), kInitialIntValue);
        ASSERT_EQ(cspTest.getStrValue(), kInitialStrValue);

        cspTest = sp->getValue(kTenantId);
        ASSERT_EQ(cspTest.getIntValue(), kInitialTenantIntValue);
        ASSERT_EQ(cspTest.getStrValue(), kInitialTenantStrValue);

        return {doc, docT};
    }

    // Asserts that a given action is ignore anywhere outside of cluster parameter collections.
    template <typename F>
    void assertIgnoredOtherNamespaces(F fn, const boost::optional<TenantId>& tenantId) {
        for (const auto& nss : kIgnoredNamespaces) {
            assertIgnored(nss, fn, tenantId);
        }
    }

    // Asserts that a given action is ignored anywhere, even on cluster parameter collections.
    template <typename F>
    void assertIgnoredAlways(F fn, const boost::optional<TenantId>& tenantId) {
        assertIgnoredOtherNamespaces(fn, tenantId);
        assertIgnored(NamespaceString::makeClusterParametersNSS(boost::none), fn, tenantId);
        assertIgnored(NamespaceString::makeClusterParametersNSS(kTenantId), fn, tenantId);
    }

    void assertParameterState(int line,
                              const boost::optional<TenantId>& tenantId,
                              int intVal,
                              StringData strVal,
                              boost::optional<LogicalTime> cpt = boost::none) {
        auto* sp =
            ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
        try {
            if (cpt) {
                ASSERT_EQ(sp->getClusterParameterTime(tenantId), *cpt);
            }

            ClusterServerParameterTest cspTest = sp->getValue(tenantId);
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
    const auto initialLogicalTime = sp->getClusterParameterTime(boost::none);
    const auto singleLogicalTime = initialLogicalTime.addTicks(1);
    const auto singleIntValue = sp->getValue(boost::none).getIntValue() + 1;
    const auto singleStrValue = "OnInsertRecord.single";

    ASSERT_LT(initialLogicalTime, singleLogicalTime);
    doInserts(NamespaceString::kClusterParametersNamespace,
              {makeClusterParametersDoc(singleLogicalTime, singleIntValue, singleStrValue)});

    ASSERT_PARAMETER_STATE(boost::none, singleIntValue, singleStrValue, singleLogicalTime);
    ASSERT_PARAMETER_STATE(kTenantId, kInitialTenantIntValue, kInitialTenantStrValue);

    // Multi-record insert.
    const auto multiLogicalTime = singleLogicalTime.addTicks(1);
    const auto multiIntValue = singleIntValue + 1;
    const auto multiStrValue = "OnInsertRecord.multi";

    ASSERT_LT(singleLogicalTime, multiLogicalTime);
    doInserts(NamespaceString::kClusterParametersNamespace,
              {
                  BSON(ClusterServerParameter::k_idFieldName << "ignored"),
                  makeClusterParametersDoc(multiLogicalTime, multiIntValue, multiStrValue),
                  BSON(ClusterServerParameter::k_idFieldName << "alsoIgnored"),
              });

    ASSERT_PARAMETER_STATE(boost::none, multiIntValue, multiStrValue, multiLogicalTime);
    ASSERT_PARAMETER_STATE(kTenantId, kInitialTenantIntValue, kInitialTenantStrValue);

    // Insert plausible records to namespaces we don't care about.
    assertIgnoredOtherNamespaces(
        [this](const auto& nss) {
            doInserts(nss, {makeClusterParametersDoc(LogicalTime(), 42, "yellow")});
        },
        boost::none);
    // Plausible on other NS, multi-insert.
    assertIgnoredOtherNamespaces(
        [this](const auto& nss) {
            auto d0 = makeClusterParametersDoc(LogicalTime(), 123, "red");
            auto d1 = makeClusterParametersDoc(LogicalTime(), 234, "green");
            auto d2 = makeClusterParametersDoc(LogicalTime(), 345, "blue");
            doInserts(nss, {d0, d1, d2});
        },
        boost::none);

    // Unknown CSP record ignored on all namespaces.
    assertIgnoredAlways(
        [this](const auto& nss) {
            doInserts(nss,
                      {BSON("_id"
                            << "ignored")});
        },
        boost::none);
    // Unknown CSP, multi-insert.
    assertIgnoredAlways(
        [this](const auto& nss) {
            doInserts(nss,
                      {BSON("_id"
                            << "ignored"),
                       BSON("_id"
                            << "also-ingored")});
        },
        boost::none);

    // Insert on separate tenant.
    const auto tenantLogicalTime = multiLogicalTime.addTicks(1);
    const auto tenantIntValue = multiIntValue + 1;
    const auto tenantStrValue = "OnInsertRecord.tenant";

    doInserts(NamespaceString::makeClusterParametersNSS(kTenantId),
              {makeClusterParametersDoc(tenantLogicalTime, tenantIntValue, tenantStrValue)});

    ASSERT_PARAMETER_STATE(boost::none, multiIntValue, multiStrValue, multiLogicalTime);
    ASSERT_PARAMETER_STATE(kTenantId, tenantIntValue, tenantStrValue, tenantLogicalTime);
}

TEST_F(ClusterServerParameterOpObserverTest, OnUpdateRecord) {
    initializeState();
    auto* sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);

    // Single record update.
    const auto initialLogicalTime = sp->getClusterParameterTime(boost::none);
    const auto singleLogicalTime = initialLogicalTime.addTicks(1);
    const auto singleIntValue = sp->getValue(boost::none).getIntValue() + 1;
    const auto singleStrValue = "OnUpdateRecord.single";
    ASSERT_LT(initialLogicalTime, singleLogicalTime);

    doUpdate(NamespaceString::kClusterParametersNamespace,
             makeClusterParametersDoc(singleLogicalTime, singleIntValue, singleStrValue));

    ASSERT_PARAMETER_STATE(boost::none, singleIntValue, singleStrValue, singleLogicalTime);
    ASSERT_PARAMETER_STATE(kTenantId, kInitialTenantIntValue, kInitialTenantStrValue);

    // Plausible doc in wrong namespace.
    assertIgnoredOtherNamespaces(
        [this](const auto& nss) {
            doUpdate(nss, makeClusterParametersDoc(LogicalTime(), 123, "ignored"));
        },
        boost::none);

    // Non cluster parameter doc.
    assertIgnoredAlways(
        [this](const auto& nss) {
            doUpdate(nss, BSON(ClusterServerParameter::k_idFieldName << "ignored"));
        },
        boost::none);

    // Update on separate tenant.
    const auto tenantLogicalTime = singleLogicalTime.addTicks(1);
    const auto tenantIntValue = singleIntValue + 1;
    const auto tenantStrValue = "OnInsertRecord.tenant";

    doUpdate(NamespaceString::makeClusterParametersNSS(kTenantId),
             makeClusterParametersDoc(tenantLogicalTime, tenantIntValue, tenantStrValue));

    ASSERT_PARAMETER_STATE(boost::none, singleIntValue, singleStrValue, singleLogicalTime);
    ASSERT_PARAMETER_STATE(kTenantId, tenantIntValue, tenantStrValue, tenantLogicalTime);
}

TEST_F(ClusterServerParameterOpObserverTest, onDeleteRecord) {
    auto* sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);

    const auto [initialDocB, initialDocT] = initializeState();
    // Structured bindings cannot be captured, move to variable.
    const auto initialDoc = std::move(initialDocB);

    // Ignore deletes in other namespaces, whether with or without deleted doc.
    assertIgnoredOtherNamespaces([this, initialDoc](const auto& nss) { doDelete(nss, initialDoc); },
                                 boost::none);

    // Ignore deletes where the _id does not correspond to a known cluster server parameter.
    assertIgnoredAlways(
        [this](const auto& nss) {
            doDelete(nss, BSON(ClusterServerParameter::k_idFieldName << "ignored"));
        },
        boost::none);

    // Reset configuration to defaults when we claim to have deleted the doc.
    doDelete(NamespaceString::kClusterParametersNamespace, initialDoc);

    ASSERT_PARAMETER_STATE(boost::none, kDefaultIntValue, kDefaultStrValue);
    ASSERT_PARAMETER_STATE(kTenantId, kInitialTenantIntValue, kInitialTenantStrValue);

    // Restore configured state, and delete without including deleteDoc reference.
    initializeState();
    doDelete(NamespaceString::kClusterParametersNamespace, initialDoc, false);

    ASSERT_PARAMETER_STATE(boost::none, kDefaultIntValue, kDefaultStrValue);
    ASSERT_PARAMETER_STATE(kTenantId, kInitialTenantIntValue, kInitialTenantStrValue);

    // Restore configured state, and delete other tenant with reference.
    initializeState();
    doDelete(NamespaceString::makeClusterParametersNSS(kTenantId), initialDocT);

    ASSERT_PARAMETER_STATE(boost::none, kInitialIntValue, kInitialStrValue);
    ASSERT_PARAMETER_STATE(kTenantId, kDefaultIntValue, kDefaultStrValue);

    // Restore and delete without reference.
    initializeState();
    doDelete(NamespaceString::makeClusterParametersNSS(kTenantId), initialDocT, false);

    ASSERT_PARAMETER_STATE(boost::none, kInitialIntValue, kInitialStrValue);
    ASSERT_PARAMETER_STATE(kTenantId, kDefaultIntValue, kDefaultStrValue);
}

TEST_F(ClusterServerParameterOpObserverTest, onDropDatabase) {
    initializeState();

    // Drop ignorable databases.
    assertIgnoredOtherNamespaces(
        [this](const auto& nss) {
            const auto dbname = nss.dbName();
            if (dbname.db() != kConfigDB) {
                doDropDatabase(dbname);
            }
        },
        boost::none);

    // Actually drop the config DB.
    doDropDatabase(DatabaseName::kConfig);

    auto* sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);

    ASSERT_PARAMETER_STATE(boost::none, kDefaultIntValue, kDefaultStrValue);
    ASSERT_PARAMETER_STATE(kTenantId, kInitialTenantIntValue, kInitialTenantStrValue);

    // Reinitialize and drop the other tenant's config DB.
    initializeState();
    doDropDatabase(DatabaseName::createDatabaseName_forTest(kTenantId, kConfigDB));

    ASSERT_PARAMETER_STATE(boost::none, kInitialIntValue, kInitialStrValue);
    ASSERT_PARAMETER_STATE(kTenantId, kDefaultIntValue, kDefaultStrValue);
}

TEST_F(ClusterServerParameterOpObserverTest, onReplicationRollback) {
    initializeState();

    const NamespaceString kTestFoo = NamespaceString::createNamespaceString_forTest("test", "foo");
    // Import ignorable collections.
    assertIgnoredOtherNamespaces([&](const auto& nss) { doReplicationRollback({nss}); },
                                 boost::none);

    auto* sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);

    remove(boost::none);
    remove(kTenantId);
    // Trigger rollback of ignorable namespaces and ensure no disk reload occurs.
    doReplicationRollback(kIgnoredNamespaces);
    ASSERT_PARAMETER_STATE(boost::none, kInitialIntValue, kInitialStrValue);
    ASSERT_PARAMETER_STATE(kTenantId, kInitialTenantIntValue, kInitialTenantStrValue);

    // Trigger rollback of relevant namespace and ensure disk reload occurs only for that tenant.
    doReplicationRollback({NamespaceString::kClusterParametersNamespace});

    ASSERT_PARAMETER_STATE(boost::none, kDefaultIntValue, kDefaultStrValue);
    ASSERT_PARAMETER_STATE(kTenantId, kInitialTenantIntValue, kInitialTenantStrValue);
    // Trigger rollback of other tenant's namespace and ensure disk reload occurs only for that
    // tenant.
    auto doc = makeClusterParametersDoc(
        LogicalTime(Timestamp(time(nullptr))), 444, "onReplicationRollback");
    upsert(doc, boost::none);
    doReplicationRollback({NamespaceString::makeClusterParametersNSS(kTenantId)});

    ASSERT_PARAMETER_STATE(boost::none, kDefaultIntValue, kDefaultStrValue);
    ASSERT_PARAMETER_STATE(kTenantId, kDefaultIntValue, kDefaultStrValue);
}

TEST_F(ClusterServerParameterOpObserverTest, abortsAfterObservation) {

    const auto [initialDocB, initialDocT] = initializeState();
    // Structured bindings cannot be captured, move to variable.
    const auto initialDoc = std::move(initialDocB);
    const auto initialDocTenant = std::move(initialDocT);

    doInserts(NamespaceString::kClusterParametersNamespace,
              {makeClusterParametersDoc(LogicalTime(Timestamp(12345678)), 123, "abc")},
              false /* commit */);
    doInserts(NamespaceString::makeClusterParametersNSS(kTenantId),
              {makeClusterParametersDoc(LogicalTime(Timestamp(23456789)), 456, "def")},
              false /* commit */);

    ASSERT_PARAMETER_STATE(boost::none, kInitialIntValue, kInitialStrValue);
    ASSERT_PARAMETER_STATE(kTenantId, kInitialTenantIntValue, kInitialTenantStrValue);

    doUpdate(NamespaceString::kClusterParametersNamespace,
             {makeClusterParametersDoc(LogicalTime(Timestamp(87654321)), 321, "cba")},
             false /* commit */);
    doUpdate(NamespaceString::makeClusterParametersNSS(kTenantId),
             {makeClusterParametersDoc(LogicalTime(Timestamp(98765432)), 654, "fed")},
             false /* commit */);

    ASSERT_PARAMETER_STATE(boost::none, kInitialIntValue, kInitialStrValue);
    ASSERT_PARAMETER_STATE(kTenantId, kInitialTenantIntValue, kInitialTenantStrValue);

    doDelete(NamespaceString::kClusterParametersNamespace,
             initialDoc,
             true /* includeDeletedDoc */,
             false /* commit */);
    doDelete(NamespaceString::makeClusterParametersNSS(kTenantId),
             initialDocTenant,
             true /* includeDeletedDoc */,
             false /* commit */);

    ASSERT_PARAMETER_STATE(boost::none, kInitialIntValue, kInitialStrValue);
    ASSERT_PARAMETER_STATE(kTenantId, kInitialTenantIntValue, kInitialTenantStrValue);

    doDropDatabase(DatabaseName::createDatabaseName_forTest(boost::none, kConfigDB),
                   false /* commit */);
    doDropDatabase(DatabaseName::createDatabaseName_forTest(kTenantId, kConfigDB),
                   false /* commit */);

    ASSERT_PARAMETER_STATE(boost::none, kInitialIntValue, kInitialStrValue);
    ASSERT_PARAMETER_STATE(kTenantId, kInitialTenantIntValue, kInitialTenantStrValue);
}

#undef ASSERT_PARAMETER_STATE

}  // namespace
}  // namespace mongo
