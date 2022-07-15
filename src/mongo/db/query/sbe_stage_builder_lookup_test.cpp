/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

/**
 * This file contains tests for building execution stages that implement $lookup operator.
 */

#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_stage_builder_test_fixture.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/util/assert_util.h"

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_test_service_context.h"

namespace mongo::sbe {
using namespace value;

class LookupStageBuilderTest : public SbeStageBuilderTestFixture {
protected:
    // Set to "true" and recompile the tests to dump plans and skip asserts in favor of printing
    // more data. Because asserts are suppressed in this mode tests might pass while being broken.
    // Do not check in with 'enableDebugOutput' set to "true".
    bool enableDebugOutput = false;

public:
    void setUp() override {
        SbeStageBuilderTestFixture::setUp();

        // Create local and foreign collections.
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), _nss, CollectionOptions()));
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), _foreignNss, CollectionOptions()));
    }

    void tearDown() override {
        _localCollLock.reset();
        _foreignCollLock.reset();
        SbeStageBuilderTestFixture::tearDown();
    }

    void insertDocuments(const NamespaceString& nss,
                         std::unique_ptr<AutoGetCollection>& lock,
                         const std::vector<BSONObj>& docs) {
        std::vector<InsertStatement> inserts{docs.begin(), docs.end()};
        lock = std::make_unique<AutoGetCollection>(operationContext(), nss, LockMode::MODE_X);
        {
            WriteUnitOfWork wuow{operationContext()};
            ASSERT_OK(
                lock.get()
                    ->getWritableCollection(operationContext())
                    ->insertDocuments(
                        operationContext(), inserts.begin(), inserts.end(), nullptr /* opDebug */));
            wuow.commit();
        }

        // Before we read, lock the collection in MODE_IS.
        lock = std::make_unique<AutoGetCollection>(operationContext(), nss, LockMode::MODE_IS);
    }

    void insertDocuments(const std::vector<BSONObj>& localDocs,
                         const std::vector<BSONObj>& foreignDocs) {
        insertDocuments(_nss, _localCollLock, localDocs);
        insertDocuments(_foreignNss, _foreignCollLock, foreignDocs);

        _collections = MultipleCollectionAccessor(operationContext(),
                                                  &_localCollLock->getCollection(),
                                                  _nss,
                                                  false /* isAnySecondaryNamespaceAViewOrSharded */,
                                                  {_foreignNss});
    }

    struct CompiledTree {
        std::unique_ptr<sbe::PlanStage> stage;
        stage_builder::PlanStageData data;
        std::unique_ptr<CompileCtx> ctx;
        SlotAccessor* resultSlotAccessor;
    };

    // Constructs ready-to-execute SBE tree for $lookup specified by the arguments.
    CompiledTree buildLookupSbeTree(EqLookupNode::LookupStrategy strategy,
                                    const std::string& localKey,
                                    const std::string& foreignKey,
                                    const std::string& asKey) {
        // Documents from the local collection are provided using collection scan.
        auto localScanNode = std::make_unique<CollectionScanNode>();
        localScanNode->name = _nss.toString();

        // Construct logical query solution.
        auto lookupNode = std::make_unique<EqLookupNode>(std::move(localScanNode),
                                                         _foreignNss,
                                                         localKey,
                                                         foreignKey,
                                                         asKey,
                                                         strategy,
                                                         boost::none /* idxEntry */,
                                                         true /* shouldProduceBson */);
        auto solution = makeQuerySolution(std::move(lookupNode));

        // Convert logical solution into the physical SBE plan.
        auto [resultSlots, stage, data, _] = buildPlanStage(std::move(solution),
                                                            false /*hasRecordId*/,
                                                            nullptr /*shard filterer*/,
                                                            nullptr /*collator*/);

        if (enableDebugOutput) {
            std::cout << std::endl << DebugPrinter{true}.print(stage->debugPrint()) << std::endl;
        }

        // Prepare the SBE tree for execution.
        auto ctx = makeCompileCtx();
        prepareTree(ctx.get(), stage.get());

        auto resultSlot = data.outputs.get(stage_builder::PlanStageSlots::kResult);
        SlotAccessor* resultSlotAccessor = stage->getAccessor(*ctx, resultSlot);

        return CompiledTree{std::move(stage), std::move(data), std::move(ctx), resultSlotAccessor};
    }

    // Check that SBE plan for '$lookup' returns expected documents.
    void assertReturnedDocuments(EqLookupNode::LookupStrategy strategy,
                                 const std::string& localKey,
                                 const std::string& foreignKey,
                                 const std::string& asKey,
                                 const std::vector<BSONObj>& expected) {
        if (enableDebugOutput) {
            std::cout << std::endl
                      << "LookupStrategy: " << EqLookupNode::serializeLookupStrategy(strategy)
                      << std::endl;
        }

        auto tree = buildLookupSbeTree(strategy, localKey, foreignKey, asKey);
        auto& stage = tree.stage;

        size_t i = 0;
        for (auto state = stage->getNext(); state == PlanState::ADVANCED;
             state = stage->getNext(), i++) {
            // Retrieve the result document from SBE plan.
            auto [resultTag, resultValue] = tree.resultSlotAccessor->copyOrMoveValue();
            ValueGuard resultGuard{resultTag, resultValue};
            if (enableDebugOutput) {
                std::cout << "Actual document:   " << std::make_pair(resultTag, resultValue)
                          << std::endl;
            }

            // If the plan returned more documents than expected, proceed extracting all of them.
            // This way, the developer will see them if debug print is enabled.
            if (i >= expected.size()) {
                continue;
            }

            // Construct view to the expected document.
            auto [expectedTag, expectedValue] =
                copyValue(TypeTags::bsonObject, bitcastFrom<const char*>(expected[i].objdata()));
            ValueGuard expectedGuard{expectedTag, expectedValue};
            if (enableDebugOutput) {
                std::cout << "Expected document: " << std::make_pair(expectedTag, expectedValue)
                          << std::endl;
            }

            // Assert that the document from SBE plan is equal to the expected one.
            assertValuesEqual(resultTag, resultValue, expectedTag, expectedValue);
        }

        ASSERT_EQ(i, expected.size());
        stage->close();
    }

    void assertReturnedDocuments(const std::string& localKey,
                                 const std::string& foreignKey,
                                 const std::string& asKey,
                                 const std::vector<BSONObj>& expected) {
        for (auto strategy : strategies) {
            assertReturnedDocuments(strategy, localKey, foreignKey, asKey, expected);
        }
    }

    // Check that SBE plan for '$lookup' returns expected documents. Expected documents are
    // described in pairs '(local document, matched foreign documents)'.
    void assertMatchedDocuments(
        EqLookupNode::LookupStrategy strategy,
        const std::string& localKey,
        const std::string& foreignKey,
        const std::vector<std::pair<BSONObj, std::vector<BSONObj>>>& expectedPairs) {
        const std::string resultFieldName{"result"};
        if (enableDebugOutput) {
            std::cout << std::endl
                      << "LookupStrategy: " << EqLookupNode::serializeLookupStrategy(strategy)
                      << std::endl;
        }

        // Construct expected documents.
        std::vector<BSONObj> expectedDocuments;
        expectedDocuments.reserve(expectedPairs.size());
        for (auto& [localDocument, matchedDocuments] : expectedPairs) {
            MutableDocument expectedDocument;
            expectedDocument.reset(localDocument, false /* stripMetadata */);

            std::vector<mongo::Value> matchedValues{matchedDocuments.begin(),
                                                    matchedDocuments.end()};
            expectedDocument.setField(resultFieldName, mongo::Value{matchedValues});
            const auto expectedBson = expectedDocument.freeze().toBson();

            expectedDocuments.push_back(expectedBson);
        }

        assertReturnedDocuments(strategy, localKey, foreignKey, resultFieldName, expectedDocuments);
    }

    void assertMatchedDocuments(
        const std::string& localKey,
        const std::string& foreignKey,
        const std::vector<std::pair<BSONObj, std::vector<BSONObj>>>& expectedPairs) {

        for (auto strategy : strategies) {
            assertMatchedDocuments(strategy, localKey, foreignKey, expectedPairs);
        }
    }

protected:
    std::vector<EqLookupNode::LookupStrategy> strategies = {
        EqLookupNode::LookupStrategy::kNestedLoopJoin, EqLookupNode::LookupStrategy::kHashJoin};

private:
    const NamespaceString _foreignNss{"testdb.sbe_stage_builder_foreign"};
    std::unique_ptr<AutoGetCollection> _localCollLock = nullptr;
    std::unique_ptr<AutoGetCollection> _foreignCollLock = nullptr;
};

TEST_F(LookupStageBuilderTest, NestedLoopJoin_Basic) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{_id: 0, lkey: 1}"),
        fromjson("{_id: 1, lkey: 12}"),
        fromjson("{_id: 2, lkey: 3}"),
        fromjson("{_id: 3, lkey: [1, 4]}"),
    };

    const std::vector<BSONObj> fdocs = {
        fromjson("{_id: 0, fkey: 1}"),
        fromjson("{_id: 1, fkey: 3}"),
        fromjson("{_id: 2, fkey: [1, 4, 25]}"),
        fromjson("{_id: 3, fkey: 4}"),
        fromjson("{_id: 4, fkey: [24, 25, 26]}"),
        fromjson("{_id: 5, no_fkey: true}"),
        fromjson("{_id: 6, fkey: null}"),
        fromjson("{_id: 7, fkey: undefined}"),
        fromjson("{_id: 8, fkey: []}"),
        fromjson("{_id: 9, fkey: [null]}"),
    };

    const std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0], fdocs[2]}},
        {ldocs[1], {}},
        {ldocs[2], {fdocs[1]}},
        {ldocs[3], {fdocs[0], fdocs[2], fdocs[3]}},
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("lkey", "fkey", expected);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_MatchingArrayAsValue) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{_id: 0, lkey: [1, 2]}"),
        fromjson("{_id: 1, lkey: [[1, 2], 3]}"),
    };

    const std::vector<BSONObj> fdocs = {
        fromjson("{_id: 0, fkey: 1}"),
        fromjson("{_id: 1, fkey: [1, 2]}"),
        fromjson("{_id: 2, fkey: [[1, 2], 42]}"),
    };

    const std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0], fdocs[1]}},
        {ldocs[1], {fdocs[1], fdocs[2]}},
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("lkey", "fkey", expected);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_TopLevelLocalField_Null) {
    const std::vector<BSONObj> ldocs = {fromjson("{_id: 0, lkey: null}")};

    const std::vector<BSONObj> fdocs = {fromjson("{_id: 0, fkey: 1}"),
                                        fromjson("{_id: 1, no_fkey: true}"),
                                        fromjson("{_id: 2, fkey: null}"),
                                        fromjson("{_id: 3, fkey: [null]}"),
                                        fromjson("{_id: 4, fkey: undefined}"),
                                        fromjson("{_id: 5, fkey: [undefined]}"),
                                        fromjson("{_id: 6, fkey: []}"),
                                        fromjson("{_id: 7, fkey: [[]]}")};

    std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected{
        {ldocs[0],
         {fdocs[1], fdocs[2], fdocs[3],
          /*fdocs[4], fdocs[5] - match in classic, but for undefined we don't care*/}},
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("lkey", "fkey", expected);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_TopLevelLocalField_Missing) {
    const std::vector<BSONObj> ldocs = {fromjson("{_id: 0, no_lkey: true}")};

    const std::vector<BSONObj> fdocs = {fromjson("{_id: 0, fkey: 1}"),
                                        fromjson("{_id: 1, no_fkey: true}"),
                                        fromjson("{_id: 2, fkey: null}"),
                                        fromjson("{_id: 3, fkey: [null]}"),
                                        fromjson("{_id: 4, fkey: undefined}"),
                                        fromjson("{_id: 5, fkey: [undefined]}"),
                                        fromjson("{_id: 6, fkey: []}"),
                                        fromjson("{_id: 7, fkey: [[]]}")};

    std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0],
         {fdocs[1], fdocs[2], fdocs[3],
          /*fdocs[4], fdocs[5] - match in classic, but for undefined we don't care*/}},
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("lkey", "fkey", expected);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_TopLevelFields_EmptyArrays) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{_id: 0, lkey: []}"),
        fromjson("{_id: 1, lkey: [[]]}"),
    };
    const std::vector<BSONObj> fdocs = {
        fromjson("{_id: 0, fkey: 1}"),
        fromjson("{_id: 1, no_fkey: true}"),
        fromjson("{_id: 2, fkey: null}"),
        fromjson("{_id: 3, fkey: [null]}"),
        fromjson("{_id: 4, fkey: undefined}"),
        fromjson("{_id: 5, fkey: [undefined]}"),
        fromjson("{_id: 6, fkey: []}"),
        fromjson("{_id: 7, fkey: [[]]}"),
    };

    std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[1], fdocs[2], fdocs[3]}},
        {ldocs[1], {fdocs[6], fdocs[7]}},
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("lkey", "fkey", expected);
}

// See 'testMatchingPathToScalar()' in lookup_equijoin_semantics.js
const std::vector<BSONObj> MatchingPathToScalar_Docs = {
    fromjson("{_id: 0, a: {x: 1, y: 2}}"),
    fromjson("{_id: 1, a: [{x: 1}, {x: 2}]}"),
    fromjson("{_id: 2, a: [{x: 1}, {x: null}]}"),
    fromjson("{_id: 3, a: [{x: 1}, {x: []}]}"),
    fromjson("{_id: 4, a: [{x: 1}, {no_x: 2}]}"),
    fromjson("{_id: 5, a: {x: [1, 2]}}"),
    fromjson("{_id: 6, a: [{x: [1, 2]}]}"),
    fromjson("{_id: 7, a: [{x: [1, 2]}, {no_x: 2}]}"),

    // For these docs "a.x" should resolve to a (logical) set that does _not_ contain value "1".
    fromjson("{_id: 8, a: {x: 2, y: 1}}"),
    fromjson("{_id: 9, a: {x: [2, 3], y: 1}}"),
    fromjson("{_id: 10, a: [{no_x: 1}, {x: 2}, {x: 3}]}"),
    fromjson("{_id: 11, a: {x: [[1], 2]}}"),
    fromjson("{_id: 12, a: [{x: [[1], 2]}]}"),
    fromjson("{_id: 13, a: {no_x: 1}}"),
};
TEST_F(LookupStageBuilderTest, NestedLoopJoin_MatchingLocalPathToForeignScalar) {
    const std::vector<BSONObj> fdocs = {fromjson("{_id: 0, fkey: 1}")};

    const std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {MatchingPathToScalar_Docs[0], {fdocs[0]}},
        {MatchingPathToScalar_Docs[1], {fdocs[0]}},
        {MatchingPathToScalar_Docs[2], {fdocs[0]}},
        {MatchingPathToScalar_Docs[3], {fdocs[0]}},
        {MatchingPathToScalar_Docs[4], {fdocs[0]}},
        {MatchingPathToScalar_Docs[5], {fdocs[0]}},
        {MatchingPathToScalar_Docs[6], {fdocs[0]}},
        {MatchingPathToScalar_Docs[7], {fdocs[0]}},

        {MatchingPathToScalar_Docs[8], {}},
        {MatchingPathToScalar_Docs[9], {}},
        {MatchingPathToScalar_Docs[10], {}},
        {MatchingPathToScalar_Docs[11], {}},
        {MatchingPathToScalar_Docs[12], {}},
        {MatchingPathToScalar_Docs[13], {}},
    };

    insertDocuments(MatchingPathToScalar_Docs, fdocs);
    assertMatchedDocuments("a.x", "fkey", expected);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_MatchingForeignPathToLocalScalar) {
    const std::vector<BSONObj> ldocs = {fromjson("{_id: 0, lkey: 1}")};

    const std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0],
         {
             MatchingPathToScalar_Docs[0],
             MatchingPathToScalar_Docs[1],
             MatchingPathToScalar_Docs[2],
             MatchingPathToScalar_Docs[3],
             MatchingPathToScalar_Docs[4],
             MatchingPathToScalar_Docs[5],
             MatchingPathToScalar_Docs[6],
             MatchingPathToScalar_Docs[7],
         }},
    };

    insertDocuments(ldocs, MatchingPathToScalar_Docs);
    assertMatchedDocuments("lkey", "a.x", expected);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_DeepLocalPath_Basic) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{_id: 0, a: [{b: [{c: 1}, {c: [[21, 22], 2]}]}, {b: {c: [[23, 24], 3]}}]}"),
    };

    const std::vector<BSONObj> fdocs = {
        fromjson("{_id: 0, fkey: 1}"),
        fromjson("{_id: 1, fkey: 2}"),
        fromjson("{_id: 2, fkey: 3}"),
        fromjson("{_id: 3, fkey: 21}"),
        fromjson("{_id: 4, fkey: 23}"),
        fromjson("{_id: 5, fkey: [21, 22]}"),
        fromjson("{_id: 6, fkey: [23, 24]}"),
    };

    const std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0], fdocs[1], fdocs[2], fdocs[5], fdocs[6]}},
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("a.b.c", "fkey", expected);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_DeepForeignPath_Basic) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{_id: 0, key: 1}"),
        fromjson("{_id: 1, key: 3}"),
        fromjson("{_id: 2, key: 4}"),
        fromjson("{_id: 3, key: 5}"),
        fromjson("{_id: 4, key: [[2, 3], 21]}"),
        fromjson("{_id: 5, key: [[4, 5], 21]}"),
        fromjson("{_id: 6, key: [[5, 4], 21]}"),
    };

    const std::vector<BSONObj> fdocs = {
        fromjson("{_id: 0, a: [ {b: [ {c: 1}, {c: [2, 3]} ]}, {b: {c: [[4, 5], 4]}} ]}"),
    };

    const std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0]}},
        {ldocs[1], {fdocs[0]}},
        {ldocs[2], {fdocs[0]}},
        {ldocs[3], {}},
        {ldocs[4], {fdocs[0]}},
        {ldocs[5], {fdocs[0]}},
        {ldocs[6], {}},
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("key", "a.b.c", expected);
}

const std::vector<BSONObj> DocsWithScalarsOnPath = {
    fromjson("{_id: 0, a: 42}"),
    fromjson("{_id: 1, no_a: 42}"),
    fromjson("{_id: 2, a: [42]}"),

    fromjson("{_id: 3, a: {b: 42}}"),
    fromjson("{_id: 4, a: {no_b: 42}}"),
    fromjson("{_id: 5, a: {b: [42]}}"),

    fromjson("{_id: 6, a: [{b: {no_c: 1}}, 1]}"),
    fromjson("{_id: 7, a: {b: [{no_c: 1}, 1]}}"),
};
TEST_F(LookupStageBuilderTest, NestedLoopJoin_DeepLocalPath_ScalarsOnPath) {
    const auto& ldocs = DocsWithScalarsOnPath;
    const std::vector<BSONObj> fdocs = {fromjson("{_id: 0, key: null}")};
    const std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0]}},
        {ldocs[1], {fdocs[0]}},
        {ldocs[2], {fdocs[0]}},
        {ldocs[3], {fdocs[0]}},
        {ldocs[4], {fdocs[0]}},
        {ldocs[5], {fdocs[0]}},
        {ldocs[6], {fdocs[0]}},
        {ldocs[7], {fdocs[0]}},
    };

    insertDocuments(DocsWithScalarsOnPath, fdocs);
    assertMatchedDocuments("a.b.c", "key", expected);
}
TEST_F(LookupStageBuilderTest, NestedLoopJoin_DeepForeignPath_ScalarsOnPath) {
    const std::vector<BSONObj> ldocs = {fromjson("{_id: 0, key: null}")};
    const auto& fdocs = DocsWithScalarsOnPath;
    const std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0],
         {
             fdocs[0],
             fdocs[1],
             // fdocs[2], is expected and different from matching in local!
             fdocs[3],
             fdocs[4],
             // fdocs[5], is expected and different from matching in local!
             fdocs[6],
             fdocs[7],
         }},
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("key", "a.b.c", expected);
}

const std::vector<BSONObj> DocsWithMissingTerminal = {
    fromjson("{_id: 0, a: {b: [ 42, {no_c: 42}, {c: 1}      ]}}"),
    fromjson("{_id: 1, a: {b: [ 42, {no_c: 42}, {c: [1, 2]} ]}}"),
    fromjson("{_id: 2, a: {b: [ 42, {no_c: 42}, {no_c: 42}  ]}}"),

    fromjson("{_id: 3, a: [ 42, {b: {no_c: 42}}, {b: {c: 1}}      ]}"),
    fromjson("{_id: 4, a: [ 42, {b: {no_c: 42}}, {b: {c: [1, 2]}} ]}"),
    fromjson("{_id: 5, a: [ 42, {b: {no_c: 42}}, {b: {no_c: 42}}  ]}"),

    fromjson("{_id: 6, a: [ 42, {b: [ 42, {no_c: 42}, {c: 1}     ]}, {b: {no_c: 42}} ]}"),
    fromjson("{_id: 7, a: [ 42, {b: [ 42, {no_c: 42}, {no_c: 42} ]}, {b: {c: 1}}     ]}"),
    fromjson("{_id: 8, a: [ 42, {b: [ 42, {no_c: 42}, {no_c: 42} ]}, {no_b: 42}      ]}"),
};
TEST_F(LookupStageBuilderTest, NestedLoopJoin_DeepLocalPath_MissingTerminal) {
    const auto& ldocs = DocsWithMissingTerminal;
    const std::vector<BSONObj> fdocs = {fromjson("{_id: 0, fkey: 1}"),
                                        fromjson("{_id: 1, fkey: null}")};

    const std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0]}},
        {ldocs[1], {fdocs[0]}},
        {ldocs[2], {fdocs[1]}},

        {ldocs[3], {fdocs[0]}},
        {ldocs[4], {fdocs[0]}},
        {ldocs[5], {fdocs[1]}},

        {ldocs[6], {fdocs[0]}},
        {ldocs[7], {fdocs[0]}},
        {ldocs[8], {fdocs[1]}},
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("a.b.c", "fkey", expected);
}
TEST_F(LookupStageBuilderTest, NestedLoopJoin_DeepForeignPath_MissingTerminal) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{_id: 0, key: 1}"),
        fromjson("{_id: 1, key: null}"),
    };
    const auto& fdocs = DocsWithMissingTerminal;

    const std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0], fdocs[1], fdocs[3], fdocs[4], fdocs[6], fdocs[7]}},
        {ldocs[1],
         {fdocs[0],
          fdocs[1],
          fdocs[2],
          fdocs[3],
          fdocs[4],
          fdocs[5],
          fdocs[6],
          fdocs[7],
          fdocs[8]}},
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("key", "a.b.c", expected);
}

const std::vector<BSONObj> DocsWithMissingOnPath = {
    fromjson("{_id: 0, no_a: {x: 42}}"),
    fromjson("{_id: 1, a: {no_b: {x: 42}}}"),
    fromjson("{_id: 2, a: [ 42, {no_b: {x: 42}}              ]}"),
    fromjson("{_id: 3, a: [ 42, {no_b: {x: 42}}, {b: {c: 1}} ]}"),
};
TEST_F(LookupStageBuilderTest, NestedLoopJoin_DeepLocalPath_MissingOnPath) {
    const auto& ldocs = DocsWithMissingOnPath;
    const std::vector<BSONObj> fdocs = {fromjson("{_id: 0, fkey: null}"),
                                        fromjson("{_id: 1, fkey: 1}")};

    const std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0]}},
        {ldocs[1], {fdocs[0]}},
        {ldocs[2], {fdocs[0]}},
        {ldocs[3], {fdocs[1]}},
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("a.b.c", "fkey", expected);
}
TEST_F(LookupStageBuilderTest, NestedLoopJoin_DeepForeignPath_MissingOnPath) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{_id: 0, key: null}"),
        fromjson("{_id: 1, key: 1}"),
    };
    const auto& fdocs = DocsWithMissingOnPath;

    const std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0], fdocs[1], fdocs[2], fdocs[3]}},
        {ldocs[1], {fdocs[3]}},
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("key", "a.b.c", expected);
}

const std::vector<BSONObj> DocsWithEmptyArraysInTerminal = {
    fromjson("{_id: 0, a: {b: {c: []}}}"),
    fromjson("{_id: 1, a: {b: [ {c: []}, {c: []}             ]}}"),
    fromjson("{_id: 2, a: {b: [ {no_c: 42}, {c: []}, {c: []} ]}}"),
    fromjson("{_id: 3, a: [ {b: {c: []}}, {b: {c: []}}                  ]}"),
    fromjson("{_id: 4, a: [ {b: {no_c: 42}}, {b: {c: []}}, {b: {c: []}} ]}"),

    fromjson("{_id: 5, a: {b: {c: [[]]}}}"),
    fromjson("{_id: 6, a: {b: [ {c: 42}, {c: [[]]}             ]}}"),
    fromjson("{_id: 7, a: {b: [ {no_c: 42}, {c: 42}, {c: [[]]} ]}}"),
    fromjson("{_id: 8, a: [ {b: {c: 42}}, {b: {c: [[]]}}                  ]}"),
    fromjson("{_id: 9, a: [ {b: {no_c: 42}}, {b: {c: 42}}, {b: {c: [[]]}} ]}"),
};
TEST_F(LookupStageBuilderTest, NestedLoopJoin_DeepLocalPath_EmptyArrays) {
    const auto& ldocs = DocsWithEmptyArraysInTerminal;
    const std::vector<BSONObj> fdocs = {fromjson("{_id: 0, fkey: null}"),
                                        fromjson("{_id: 1, fkey: [[]]}")};

    std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0]}},
        {ldocs[1], {fdocs[0]}},
        {ldocs[2], {fdocs[0]}},
        {ldocs[3], {fdocs[0]}},
        {ldocs[4], {fdocs[0]}},

        {ldocs[5], {fdocs[1]}},
        {ldocs[6], {fdocs[1]}},
        {ldocs[7], {fdocs[1]}},
        {ldocs[8], {fdocs[1]}},
        {ldocs[9], {fdocs[1]}},
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("a.b.c", "fkey", expected);
}
TEST_F(LookupStageBuilderTest, NestedLoopJoin_DeepForeignPath_EmptyArrays) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{_id: 0, key: [[]]}"),
        fromjson("{_id: 1, key: null}"),
    };
    const auto& fdocs = DocsWithEmptyArraysInTerminal;

    std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0],
         {
             fdocs[0],
             fdocs[1],
             fdocs[2],
             fdocs[3],
             fdocs[4],
             fdocs[5],
             fdocs[6],
             fdocs[7],
             fdocs[8],
             fdocs[9],
         }},
        {ldocs[1], {fdocs[2], fdocs[4], fdocs[7], fdocs[9]}},
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("key", "a.b.c", expected);
}

TEST_F(LookupStageBuilderTest, OneComponentAsPath) {
    insertDocuments({fromjson("{_id: 0}")}, {fromjson("{_id: 0}")});

    assertReturnedDocuments("_id", "_id", "result", {fromjson("{_id: 0, result: [{_id: 0}]}")});
}

TEST_F(LookupStageBuilderTest, OneComponentAsPathReplacingExistingObject) {
    insertDocuments({fromjson("{_id: 0, result: {a: {b: 1}, c: 2}}")}, {fromjson("{_id: 0}")});

    assertReturnedDocuments("_id", "_id", "result", {fromjson("{_id: 0, result: [{_id: 0}]}")});
}

TEST_F(LookupStageBuilderTest, OneComponentAsPathReplacingExistingArray) {
    insertDocuments({fromjson("{_id: 0, result: [{a: 1}, {b: 2}]}")}, {fromjson("{_id: 0}")});

    assertReturnedDocuments("_id", "_id", "result", {fromjson("{_id: 0, result: [{_id: 0}]}")});
}

TEST_F(LookupStageBuilderTest, ThreeComponentAsPath) {
    insertDocuments({fromjson("{_id: 0}")}, {fromjson("{_id: 0}")});

    assertReturnedDocuments(
        "_id", "_id", "one.two.three", {fromjson("{_id: 0, one: {two: {three: [{_id: 0}]}}}")});
}

TEST_F(LookupStageBuilderTest, ThreeComponentAsPathExtendingExistingObjectOnOneLevel) {
    insertDocuments({fromjson("{_id: 0, one: {a: 1}}")}, {fromjson("{_id: 0}")});

    assertReturnedDocuments("_id",
                            "_id",
                            "one.two.three",
                            {fromjson("{_id: 0, one: {a: 1, two: {three: [{_id: 0}]}}}")});
}

TEST_F(LookupStageBuilderTest, ThreeComponentAsPathExtendingExistingObjectOnTwoLevels) {
    insertDocuments({fromjson("{_id: 0, one: {a: 1, two: {b: 2}}}")}, {fromjson("{_id: 0}")});

    assertReturnedDocuments("_id",
                            "_id",
                            "one.two.three",
                            {fromjson("{_id: 0, one: {a: 1, two: {b: 2, three: [{_id: 0}]}}}")});
}

TEST_F(LookupStageBuilderTest, ThreeComponentAsPathReplacingSingleValueInExistingObject) {
    insertDocuments({fromjson("{_id: 0, one: {a: 1, two: {b: 2, three: 3}}}}")},
                    {fromjson("{_id: 0}")});

    assertReturnedDocuments("_id",
                            "_id",
                            "one.two.three",
                            {fromjson("{_id: 0, one: {a: 1, two: {b: 2, three: [{_id: 0}]}}}")});
}

TEST_F(LookupStageBuilderTest, ThreeComponentAsPathReplacingExistingArray) {
    insertDocuments({fromjson("{_id: 0, one: [{a: 1}, {b: 2}]}")}, {fromjson("{_id: 0}")});

    assertReturnedDocuments(
        "_id", "_id", "one.two.three", {fromjson("{_id: 0, one: {two: {three: [{_id: 0}]}}}")});
}

TEST_F(LookupStageBuilderTest, ThreeComponentAsPathDoesNotPerformArrayTraversal) {
    insertDocuments({fromjson("{_id: 0, one: [{a: 1, two: [{b: 2, three: 3}]}]}")},
                    {fromjson("{_id: 0}")});

    assertReturnedDocuments(
        "_id", "_id", "one.two.three", {fromjson("{_id: 0, one: {two: {three: [{_id: 0}]}}}")});
}

}  // namespace mongo::sbe
