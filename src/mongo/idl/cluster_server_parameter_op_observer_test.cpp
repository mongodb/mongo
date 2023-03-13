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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/idl/cluster_server_parameter_op_observer.h"
#include "mongo/idl/cluster_server_parameter_test_util.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {
using namespace cluster_server_parameter_test_util;

const std::vector<NamespaceString> kIgnoredNamespaces = {
    NamespaceString("config"_sd, "settings"_sd),
    NamespaceString("local"_sd, "clusterParameters"_sd),
    NamespaceString("test"_sd, "foo"_sd)};

class ClusterServerParameterOpObserverTest : public ClusterServerParameterTestBase {
public:
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
        observer.onInserts(
            opCtx.get(), nss, UUID::gen(), stmts.cbegin(), stmts.cend(), false /* fromMigrate */);
        if (commit)
            wuow.commit();
    }

    void doUpdate(const NamespaceString& nss, BSONObj updatedDoc, bool commit = true) {
        // Actual UUID doesn't matter, just use any...
        CollectionUpdateArgs updateArgs;
        updateArgs.update = BSON("$set" << updatedDoc);
        updateArgs.updatedDoc = updatedDoc;
        OplogUpdateEntryArgs entryArgs(&updateArgs, nss, UUID::gen());
        auto opCtx = cc().makeOperationContext();
        WriteUnitOfWork wuow(opCtx.get());
        AutoGetCollection autoColl(opCtx.get(), nss, MODE_IX);
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
        observer.aboutToDelete(opCtx.get(), nss, UUID::gen(), deletedDoc);
        OplogDeleteEntryArgs args;
        args.deletedDoc = includeDeletedDoc ? &deletedDoc : nullptr;
        observer.onDelete(opCtx.get(), nss, UUID::gen(), 1 /* StmtId */, args);
        if (commit)
            wuow.commit();
    }

    void doDropDatabase(const std::string& dbname, bool commit = true) {
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
    void assertIgnored(const NamespaceString& nss, F fn) {
        auto* sp = ServerParameterSet::getClusterParameterSet()
                       ->get<IDLServerParameterWithStorage<ServerParameterType::kClusterWide,
                                                           ClusterServerParameterTest>>(kCSPTest);
        ASSERT(sp != nullptr);

        const auto initialCPTime = sp->getClusterParameterTime();
        ClusterServerParameterTest initialCspTest = sp->getValue();
        fn(nss);
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

        auto* sp = ServerParameterSet::getClusterParameterSet()
                       ->get<IDLServerParameterWithStorage<ServerParameterType::kClusterWide,
                                                           ClusterServerParameterTest>>(kCSPTest);
        ASSERT(sp != nullptr);

        ClusterServerParameterTest cspTest = sp->getValue();
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

    void assertParameterState(int line,
                              int intVal,
                              StringData strVal,
                              boost::optional<LogicalTime> cpt = boost::none) {
        auto* sp = ServerParameterSet::getClusterParameterSet()
                       ->get<IDLServerParameterWithStorage<ServerParameterType::kClusterWide,
                                                           ClusterServerParameterTest>>(kCSPTest);
        ;
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

TEST_F(ClusterServerParameterOpObserverTest, OnInsertRecord) {
    auto* sp = ServerParameterSet::getClusterParameterSet()
                   ->get<IDLServerParameterWithStorage<ServerParameterType::kClusterWide,
                                                       ClusterServerParameterTest>>(kCSPTest);
    ASSERT(sp != nullptr);

    // Single record insert.
    const auto initialLogicalTime = sp->getClusterParameterTime();
    const auto singleLogicalTime = initialLogicalTime.addTicks(1);
    const auto singleIntValue = sp->getValue().getIntValue() + 1;
    const auto singleStrValue = "OnInsertRecord.single";

    ASSERT_LT(initialLogicalTime, singleLogicalTime);
    doInserts(NamespaceString::kClusterParametersNamespace,
              {makeClusterParametersDoc(singleLogicalTime, singleIntValue, singleStrValue)});

    ClusterServerParameterTest cspTest = sp->getValue();
    ASSERT_EQ(sp->getClusterParameterTime(), singleLogicalTime);
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

    cspTest = sp->getValue();
    ASSERT_EQ(sp->getClusterParameterTime(), multiLogicalTime);
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
    auto* sp = ServerParameterSet::getClusterParameterSet()
                   ->get<IDLServerParameterWithStorage<ServerParameterType::kClusterWide,
                                                       ClusterServerParameterTest>>(kCSPTest);
    ASSERT(sp != nullptr);

    // Single record update.
    const auto initialLogicalTime = sp->getClusterParameterTime();
    const auto singleLogicalTime = initialLogicalTime.addTicks(1);
    const auto singleIntValue = sp->getValue().getIntValue() + 1;
    const auto singleStrValue = "OnUpdateRecord.single";
    ASSERT_LT(initialLogicalTime, singleLogicalTime);

    doUpdate(NamespaceString::kClusterParametersNamespace,
             makeClusterParametersDoc(singleLogicalTime, singleIntValue, singleStrValue));

    ClusterServerParameterTest cspTest = sp->getValue();
    ASSERT_EQ(sp->getClusterParameterTime(), singleLogicalTime);
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
    auto* sp = ServerParameterSet::getClusterParameterSet()
                   ->get<IDLServerParameterWithStorage<ServerParameterType::kClusterWide,
                                                       ClusterServerParameterTest>>(kCSPTest);
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
    ClusterServerParameterTest cspTest = sp->getValue();
    ASSERT_EQ(cspTest.getIntValue(), kDefaultIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kDefaultStrValue);


    // Restore configured state, and delete without including deleteDoc reference.
    initializeState();
    doDelete(NamespaceString::kClusterParametersNamespace, initialDoc, false);
    cspTest = sp->getValue();
    ASSERT_EQ(cspTest.getIntValue(), kDefaultIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kDefaultStrValue);
}

TEST_F(ClusterServerParameterOpObserverTest, onDropDatabase) {
    initializeState();

    // Drop ignorable databases.
    assertIgnoredOtherNamespaces([this](const auto& nss) {
        const auto dbname = nss.db();
        if (dbname != kConfigDB) {
            doDropDatabase(dbname.toString());
        }
    });

    // Actually drop the config DB.
    doDropDatabase(kConfigDB.toString());

    auto* sp = ServerParameterSet::getClusterParameterSet()
                   ->get<IDLServerParameterWithStorage<ServerParameterType::kClusterWide,
                                                       ClusterServerParameterTest>>(kCSPTest);
    ASSERT(sp != nullptr);

    ClusterServerParameterTest cspTest = sp->getValue();
    ASSERT_EQ(cspTest.getIntValue(), kDefaultIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kDefaultStrValue);
}

TEST_F(ClusterServerParameterOpObserverTest, onReplicationRollback) {
    initializeState();

    const NamespaceString kTestFoo("test", "foo");
    // Import ignorable collections.
    assertIgnoredOtherNamespaces([&](const auto& nss) { doReplicationRollback({nss}); });

    auto* sp = ServerParameterSet::getClusterParameterSet()
                   ->get<IDLServerParameterWithStorage<ServerParameterType::kClusterWide,
                                                       ClusterServerParameterTest>>(kCSPTest);
    ASSERT(sp != nullptr);

    // Trigger rollback of ignorable namespaces.
    doReplicationRollback(kIgnoredNamespaces);

    ClusterServerParameterTest cspTest = sp->getValue();
    ASSERT_EQ(cspTest.getIntValue(), kInitialIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kInitialStrValue);

    // Trigger rollback of relevant namespace.
    remove();
    doReplicationRollback({NamespaceString::kClusterParametersNamespace});
    cspTest = sp->getValue();
    ASSERT_EQ(cspTest.getIntValue(), kDefaultIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kDefaultStrValue);

    // Rollback the rollback.
    auto doc = makeClusterParametersDoc(
        LogicalTime(Timestamp(time(nullptr))), 444, "onReplicationRollback");
    upsert(doc);
    cspTest = sp->getValue();
    doReplicationRollback({NamespaceString::kClusterParametersNamespace});
    ASSERT_EQ(cspTest.getIntValue(), kDefaultIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kDefaultStrValue);
}

#define ASSERT_PARAMETER_STATE(...) assertParameterState(__LINE__, __VA_ARGS__)

TEST_F(ClusterServerParameterOpObserverTest, abortsAfterObservation) {

    const auto initialDoc = initializeState();

    doInserts(NamespaceString::kClusterParametersNamespace,
              {makeClusterParametersDoc(LogicalTime(Timestamp(12345678)), 123, "abc")},
              false /* commit */);

    ASSERT_PARAMETER_STATE(kInitialIntValue, kInitialStrValue);

    doUpdate(NamespaceString::kClusterParametersNamespace,
             {makeClusterParametersDoc(LogicalTime(Timestamp(87654321)), 321, "cba")},
             false /* commit */);

    ASSERT_PARAMETER_STATE(kInitialIntValue, kInitialStrValue);

    doDelete(NamespaceString::kClusterParametersNamespace,
             initialDoc,
             true /* includeDeletedDoc */,
             false /* commit */);

    ASSERT_PARAMETER_STATE(kInitialIntValue, kInitialStrValue);

    doDropDatabase(kConfigDB.toString(), false /* commit */);

    ASSERT_PARAMETER_STATE(kInitialIntValue, kInitialStrValue);
}

#undef ASSERT_PARAMETER_STATE

}  // namespace
}  // namespace mongo
