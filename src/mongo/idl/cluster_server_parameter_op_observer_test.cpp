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
    NamespaceString("config"_sd, "settings"_sd),
    NamespaceString("local"_sd, "clusterParameters"_sd),
    NamespaceString("test"_sd, "foo"_sd)};

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

    void doInserts(const NamespaceString& nss, std::initializer_list<BSONObj> docs) {
        std::vector<InsertStatement> stmts;
        std::transform(docs.begin(), docs.end(), std::back_inserter(stmts), [](auto doc) {
            return InsertStatement(doc);
        });
        auto opCtx = cc().makeOperationContext();

        AutoGetCollection autoColl(opCtx.get(), nss, MODE_IX);
        observer.onInserts(
            opCtx.get(), *autoColl, stmts.cbegin(), stmts.cend(), false /* fromMigrate */);
    }

    void doUpdate(const NamespaceString& nss, BSONObj updatedDoc) {
        // Actual UUID doesn't matter, just use any...
        CollectionUpdateArgs updateArgs;
        updateArgs.update = BSON("$set" << updatedDoc);
        updateArgs.updatedDoc = updatedDoc;
        OplogUpdateEntryArgs entryArgs(&updateArgs, nss, UUID::gen());
        auto opCtx = cc().makeOperationContext();
        observer.onUpdate(opCtx.get(), entryArgs);
    }

    void doDelete(const NamespaceString& nss, BSONObj deletedDoc, bool includeDeletedDoc = true) {
        auto opCtx = cc().makeOperationContext();
        auto uuid = UUID::gen();
        observer.aboutToDelete(opCtx.get(), nss, uuid, deletedDoc);
        OplogDeleteEntryArgs args;
        args.deletedDoc = includeDeletedDoc ? &deletedDoc : nullptr;
        observer.onDelete(opCtx.get(), nss, uuid, 1 /* StmtId */, args);
    }

    void doDropDatabase(StringData dbname) {
        auto opCtx = cc().makeOperationContext();
        observer.onDropDatabase(opCtx.get(), DatabaseName(boost::none, dbname));
    }

    void doRenameCollection(const NamespaceString& fromColl, const NamespaceString& toColl) {
        auto opCtx = cc().makeOperationContext();
        observer.postRenameCollection(opCtx.get(),
                                      fromColl,
                                      toColl,
                                      UUID::gen(),
                                      boost::none /* targetUUID */,
                                      false /* stayTemp */);
    }

    void doImportCollection(const NamespaceString& nss) {
        auto opCtx = cc().makeOperationContext();
        observer.onImportCollection(opCtx.get(),
                                    UUID::gen(),
                                    nss,
                                    10 /* num records */,
                                    1 << 20 /* data size */,
                                    BSONObj() /* catalogEntry */,
                                    BSONObj() /* storageMetadata */,
                                    false /* isDryRun */);
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

        const auto initialCPTime = sp->getClusterParameterTime(boost::none);
        ClusterServerParameterTest initialCspTest = sp->getValue(boost::none);
        fn(nss);
        ClusterServerParameterTest finalCspTest = sp->getValue(boost::none);

        ASSERT_EQ(sp->getClusterParameterTime(boost::none), initialCPTime);
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

        ClusterServerParameterTest cspTest = sp->getValue(boost::none);
        ASSERT_EQ(cspTest.getIntValue(), kInitialIntValue);
        ASSERT_EQ(cspTest.getStrValue(), kInitialStrValue);

        return doc;
    }

    // Asserts that a given action is ignore anywhere outside of config.clusterParameters.
    template <typename F>
    void assertIgnoredOtherNamespaces(F fn) {
        for (const auto& nss : kIgnoredNamespaces) {
            assertIgnored(nss, fn);
        }
    }

    // Asserts that a given action is ignored anywhere, even on the config.clusterParameters NS.
    template <typename F>
    void assertIgnoredAlways(F fn) {
        assertIgnoredOtherNamespaces(fn);
        assertIgnored(NamespaceString::kClusterParametersNamespace, fn);
    }

protected:
    ClusterServerParameterOpObserver observer;
};

TEST_F(ClusterServerParameterOpObserverTest, OnInsertRecord) {
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

    ClusterServerParameterTest cspTest = sp->getValue(boost::none);
    ASSERT_EQ(sp->getClusterParameterTime(boost::none), singleLogicalTime);
    ASSERT_EQ(cspTest.getIntValue(), singleIntValue);
    ASSERT_EQ(cspTest.getStrValue(), singleStrValue);

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

    cspTest = sp->getValue(boost::none);
    ASSERT_EQ(sp->getClusterParameterTime(boost::none), multiLogicalTime);
    ASSERT_EQ(cspTest.getIntValue(), multiIntValue);
    ASSERT_EQ(cspTest.getStrValue(), multiStrValue);

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

    // Unknown CSP record ignored on all namespaces.
    assertIgnoredAlways([this](const auto& nss) {
        doInserts(nss,
                  {BSON("_id"
                        << "ignored")});
    });
    // Unknown CSP, multi-insert.
    assertIgnoredAlways([this](const auto& nss) {
        doInserts(nss,
                  {BSON("_id"
                        << "ignored"),
                   BSON("_id"
                        << "also-ingored")});
    });
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

    ClusterServerParameterTest cspTest = sp->getValue(boost::none);
    ASSERT_EQ(sp->getClusterParameterTime(boost::none), singleLogicalTime);
    ASSERT_EQ(cspTest.getIntValue(), singleIntValue);
    ASSERT_EQ(cspTest.getStrValue(), singleStrValue);

    // Plausible doc in wrong namespace.
    assertIgnoredOtherNamespaces([this](const auto& nss) {
        doUpdate(nss, makeClusterParametersDoc(LogicalTime(), 123, "ignored"));
    });

    // Non cluster parameter doc.
    assertIgnoredAlways([this](const auto& nss) {
        doUpdate(nss, BSON(ClusterServerParameter::k_idFieldName << "ignored"));
    });
}

TEST_F(ClusterServerParameterOpObserverTest, onDeleteRecord) {
    auto* sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);

    const auto initialDoc = initializeState();

    // Ignore deletes in other namespaces, whether with or without deleted doc.
    assertIgnoredOtherNamespaces(
        [this, initialDoc](const auto& nss) { doDelete(nss, initialDoc); });

    // Ignore deletes where the _id does not correspond to a known cluster server parameter.
    assertIgnoredAlways([this](const auto& nss) {
        doDelete(nss, BSON(ClusterServerParameter::k_idFieldName << "ignored"));
    });

    // Reset configuration to defaults when we claim to have deleted the doc.
    doDelete(NamespaceString::kClusterParametersNamespace, initialDoc);
    ClusterServerParameterTest cspTest = sp->getValue(boost::none);
    ASSERT_EQ(cspTest.getIntValue(), kDefaultIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kDefaultStrValue);


    // Restore configured state, and delete without including deleteDoc reference.
    initializeState();
    doDelete(NamespaceString::kClusterParametersNamespace, initialDoc, false);
    cspTest = sp->getValue(boost::none);
    ASSERT_EQ(cspTest.getIntValue(), kDefaultIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kDefaultStrValue);
}

TEST_F(ClusterServerParameterOpObserverTest, onDropDatabase) {
    initializeState();

    // Drop ignorable databases.
    assertIgnoredOtherNamespaces([this](const auto& nss) {
        const auto dbname = nss.db();
        if (dbname != kConfigDB) {
            doDropDatabase(dbname);
        }
    });

    // Actually drop the config DB.
    doDropDatabase(kConfigDB);

    auto* sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);

    ClusterServerParameterTest cspTest = sp->getValue(boost::none);
    ASSERT_EQ(cspTest.getIntValue(), kDefaultIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kDefaultStrValue);
}

TEST_F(ClusterServerParameterOpObserverTest, onRenameCollection) {
    initializeState();

    const NamespaceString kTestFoo("test", "foo");
    // Rename ignorable collections.
    assertIgnoredOtherNamespaces([&](const auto& nss) { doRenameCollection(nss, kTestFoo); });
    assertIgnoredOtherNamespaces([&](const auto& nss) { doRenameCollection(kTestFoo, nss); });

    auto* sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);

    // These renames "work" despite not mutating durable state
    // since the rename away doesn't require a rescan.

    // Rename away (and reset to default)
    doRenameCollection(NamespaceString::kClusterParametersNamespace, kTestFoo);
    ClusterServerParameterTest cspTest = sp->getValue(boost::none);
    ASSERT_EQ(cspTest.getIntValue(), kDefaultIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kDefaultStrValue);

    // Rename in (and restore to initialized state)
    doRenameCollection(kTestFoo, NamespaceString::kClusterParametersNamespace);
    cspTest = sp->getValue(boost::none);
    ASSERT_EQ(cspTest.getIntValue(), kInitialIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kInitialStrValue);
}

TEST_F(ClusterServerParameterOpObserverTest, onImportCollection) {
    initializeState();

    const NamespaceString kTestFoo("test", "foo");
    // Import ignorable collections.
    assertIgnoredOtherNamespaces([&](const auto& nss) { doImportCollection(nss); });

    auto* sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);

    // Import the collection (rescan).
    auto doc =
        makeClusterParametersDoc(LogicalTime(Timestamp(time(nullptr))), 333, "onImportCollection");
    upsert(doc);
    doImportCollection(NamespaceString::kClusterParametersNamespace);
    ClusterServerParameterTest cspTest = sp->getValue(boost::none);
    ASSERT_EQ(cspTest.getIntValue(), 333);
    ASSERT_EQ(cspTest.getStrValue(), "onImportCollection");
}

TEST_F(ClusterServerParameterOpObserverTest, onReplicationRollback) {
    initializeState();

    const NamespaceString kTestFoo("test", "foo");
    // Import ignorable collections.
    assertIgnoredOtherNamespaces([&](const auto& nss) { doImportCollection(nss); });

    auto* sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);

    // Trigger rollback of ignorable namespaces.
    doReplicationRollback(kIgnoredNamespaces);

    ClusterServerParameterTest cspTest = sp->getValue(boost::none);
    ASSERT_EQ(cspTest.getIntValue(), kInitialIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kInitialStrValue);

    // Trigger rollback of relevant namespace.
    remove();
    doReplicationRollback({NamespaceString::kClusterParametersNamespace});
    cspTest = sp->getValue(boost::none);
    ASSERT_EQ(cspTest.getIntValue(), kDefaultIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kDefaultStrValue);

    // Rollback the rollback.
    auto doc = makeClusterParametersDoc(
        LogicalTime(Timestamp(time(nullptr))), 444, "onReplicationRollback");
    upsert(doc);
    cspTest = sp->getValue(boost::none);
    doReplicationRollback({NamespaceString::kClusterParametersNamespace});
    ASSERT_EQ(cspTest.getIntValue(), kDefaultIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kDefaultStrValue);
}

}  // namespace
}  // namespace mongo
