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

#include "mongo/db/update/update_driver.h"


#include <map>

#include "mongo/base/simple_string_data_comparator.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/update_index_data.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using str::stream;
using unittest::assertGet;

write_ops::UpdateModification makeUpdateMod(const BSONObj& bson) {
    return write_ops::UpdateModification::parseFromClassicUpdate(bson);
}

TEST(Parse, Normal) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_DOES_NOT_THROW(driver.parse(makeUpdateMod(fromjson("{$set:{a:1}}")), arrayFilters));
    ASSERT_FALSE(driver.type() == UpdateDriver::UpdateType::kReplacement);
}

TEST(Parse, MultiMods) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_DOES_NOT_THROW(driver.parse(makeUpdateMod(fromjson("{$set:{a:1, b:1}}")), arrayFilters));
    ASSERT_FALSE(driver.type() == UpdateDriver::UpdateType::kReplacement);
}

TEST(Parse, MixingMods) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_DOES_NOT_THROW(
        driver.parse(makeUpdateMod(fromjson("{$set:{a:1}, $unset:{b:1}}")), arrayFilters));
    ASSERT_FALSE(driver.type() == UpdateDriver::UpdateType::kReplacement);
}

TEST(Parse, ObjectReplacment) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_DOES_NOT_THROW(
        driver.parse(makeUpdateMod(fromjson("{obj: \"obj replacement\"}")), arrayFilters));
    ASSERT_TRUE(driver.type() == UpdateDriver::UpdateType::kReplacement);
}

TEST(Parse, ParseUpdateWithPipeline) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto updateObj = BSON("u" << BSON_ARRAY(BSON("$addFields" << BSON("a" << 1))));
    ASSERT_DOES_NOT_THROW(driver.parse(updateObj["u"], arrayFilters));
    ASSERT_TRUE(driver.type() == UpdateDriver::UpdateType::kPipeline);
}

TEST(Parse, ParseUpdateWithPipelineAndVariables) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    const auto variables = BSON("var1" << 1 << "var2"
                                       << "foo");
    auto updateObj = BSON("u" << BSON_ARRAY(BSON("$set" << BSON("a"
                                                                << "$$var1"
                                                                << "b"
                                                                << "$$var2"))));
    ASSERT_DOES_NOT_THROW(driver.parse(updateObj["u"], arrayFilters, variables));
    ASSERT_TRUE(driver.type() == UpdateDriver::UpdateType::kPipeline);
}

TEST(Parse, EmptyMod) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    // Verifies that {$set: {}} is accepted.
    ASSERT_DOES_NOT_THROW(driver.parse(makeUpdateMod(fromjson("{$set: {}}")), arrayFilters));
}

TEST(Parse, WrongMod) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_THROWS_CODE_AND_WHAT(driver.parse(makeUpdateMod(fromjson("{$xyz:{a:1}}")), arrayFilters),
                                AssertionException,
                                ErrorCodes::FailedToParse,
                                "Unknown modifier: $xyz. Expected a valid update modifier or "
                                "pipeline-style update specified as an array");
}

TEST(Parse, WrongType) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_THROWS_CODE_AND_WHAT(
        driver.parse(makeUpdateMod(fromjson("{$set:[{a:1}]}")), arrayFilters),
        AssertionException,
        ErrorCodes::FailedToParse,
        "Modifiers operate on fields but we found type array instead. For "
        "example: {$mod: {<field>: ...}} not {$set: [ { a: 1 } ]}");
}

TEST(Parse, ModsWithLaterObjReplacement) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_THROWS_CODE_AND_WHAT(
        driver.parse(makeUpdateMod(fromjson("{$set:{a:1}, obj: \"obj replacement\"}")),
                     arrayFilters),
        AssertionException,
        ErrorCodes::FailedToParse,
        "Unknown modifier: obj. Expected a valid update modifier or pipeline-style update "
        "specified as an array");
}

TEST(Parse, SetOnInsert) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_DOES_NOT_THROW(
        driver.parse(makeUpdateMod(fromjson("{$setOnInsert:{a:1}}")), arrayFilters));
    ASSERT_FALSE(driver.type() == UpdateDriver::UpdateType::kReplacement);
}

TEST(Collator, SetCollationUpdatesModifierInterfaces) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CollatorInterfaceMock reverseStringCollator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj updateDocument = fromjson("{$max: {a: 'abd'}}");
    UpdateDriver driver(expCtx);
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;

    ASSERT_DOES_NOT_THROW(driver.parse(makeUpdateMod(updateDocument), arrayFilters));

    const bool validateForStorage = true;
    const FieldRefSet emptyImmutablePaths;
    const bool isInsert = false;
    bool modified = false;
    mutablebson::Document doc(fromjson("{a: 'cba'}"));
    driver.setCollator(&reverseStringCollator);
    ASSERT_OK(driver.update(expCtx->opCtx,
                            StringData(),
                            &doc,
                            validateForStorage,
                            emptyImmutablePaths,
                            isInsert,
                            nullptr,
                            &modified));

    ASSERT_TRUE(modified);
}

//
// Tests of creating a base for an upsert from a query document
// $or, $and, $all get special handling, as does the _id field
//
// NONGOAL: Testing all query parsing and nesting combinations
//

class CreateFromQueryFixture : public mongo::unittest::Test {
public:
    CreateFromQueryFixture()
        : _opCtx(_serviceContext.makeOperationContext()),
          _driverOps(new UpdateDriver(
              new ExpressionContext(_opCtx.get(), nullptr, NamespaceString("foo")))),
          _driverRepl(new UpdateDriver(
              new ExpressionContext(_opCtx.get(), nullptr, NamespaceString("foo")))) {
        _driverOps->parse(makeUpdateMod(fromjson("{$set:{'_':1}}")), _arrayFilters);
        _driverRepl->parse(makeUpdateMod(fromjson("{}")), _arrayFilters);
    }

    mutablebson::Document& doc() {
        return _doc;
    }

    UpdateDriver& driverOps() {
        return *_driverOps;
    }

    UpdateDriver& driverRepl() {
        return *_driverRepl;
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

private:
    QueryTestServiceContext _serviceContext;
    ServiceContext::UniqueOperationContext _opCtx;
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> _arrayFilters;
    std::unique_ptr<UpdateDriver> _driverOps;
    std::unique_ptr<UpdateDriver> _driverRepl;
    mutablebson::Document _doc;
};

// Make name nicer to report
typedef CreateFromQueryFixture CreateFromQuery;

static void assertSameFields(const BSONObj& docA, const BSONObj& docB);

/**
 * Recursively asserts that two BSONElements contain the same data or sub-elements,
 * ignoring element order.
 */
static void assertSameElements(const BSONElement& elA, const BSONElement& elB) {
    BSONElementComparator eltCmp(BSONElementComparator::FieldNamesMode::kIgnore,
                                 &SimpleStringDataComparator::kInstance);
    if (elA.type() != elB.type() || (!elA.isABSONObj() && eltCmp.evaluate(elA != elB))) {
        FAIL(stream() << "element " << elA << " not equal to " << elB);
    } else if (elA.type() == mongo::Array) {
        std::vector<BSONElement> elsA = elA.Array();
        std::vector<BSONElement> elsB = elB.Array();
        if (elsA.size() != elsB.size())
            FAIL(stream() << "element " << elA << " not equal to " << elB);

        std::vector<BSONElement>::iterator arrItA = elsA.begin();
        std::vector<BSONElement>::iterator arrItB = elsB.begin();
        for (; arrItA != elsA.end(); ++arrItA, ++arrItB) {
            assertSameElements(*arrItA, *arrItB);
        }
    } else if (elA.type() == mongo::Object) {
        assertSameFields(elA.Obj(), elB.Obj());
    }
}

/**
 * Recursively asserts that two BSONObjects contain the same elements,
 * ignoring element order.
 */
static void assertSameFields(const BSONObj& docA, const BSONObj& docB) {
    if (docA.nFields() != docB.nFields())
        FAIL(stream() << "document " << docA << " has different fields than " << docB);

    std::map<StringData, BSONElement> docAMap;
    BSONObjIterator itA(docA);
    while (itA.more()) {
        BSONElement elA = itA.next();
        docAMap.insert(std::make_pair(elA.fieldNameStringData(), elA));
    }

    BSONObjIterator itB(docB);
    while (itB.more()) {
        BSONElement elB = itB.next();

        std::map<StringData, BSONElement>::iterator seenIt =
            docAMap.find(elB.fieldNameStringData());
        if (seenIt == docAMap.end())
            FAIL(stream() << "element " << elB << " not found in " << docA);

        BSONElement elA = seenIt->second;
        assertSameElements(elA, elB);
    }
}

TEST_F(CreateFromQuery, BasicOp) {
    BSONObj query = fromjson("{a:1,b:2}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(query, doc().getObject());
}

TEST_F(CreateFromQuery, BasicOpEq) {
    BSONObj query = fromjson("{a:{$eq:1}}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(fromjson("{a:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, BasicOpWithId) {
    BSONObj query = fromjson("{_id:1,a:1,b:2}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(query, doc().getObject());
}

TEST_F(CreateFromQuery, BasicRepl) {
    BSONObj query = fromjson("{a:1,b:2}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(fromjson("{}"), doc().getObject());
}

TEST_F(CreateFromQuery, BasicReplWithId) {
    BSONObj query = fromjson("{_id:1,a:1,b:2}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(fromjson("{_id:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, BasicReplWithIdEq) {
    BSONObj query = fromjson("{_id:{$eq:1},a:1,b:2}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(fromjson("{_id:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, NoRootIdOp) {
    BSONObj query = fromjson("{'_id.a':1,'_id.b':2}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(fromjson("{_id:{a:1,b:2}}"), doc().getObject());
}

TEST_F(CreateFromQuery, NoRootIdRepl) {
    BSONObj query = fromjson("{'_id.a':1,'_id.b':2}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_NOT_OK(
        driverRepl().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
}

TEST_F(CreateFromQuery, NestedSharedRootOp) {
    BSONObj query = fromjson("{'a.c':1,'a.b':{$eq:2}}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(fromjson("{a:{c:1,b:2}}"), doc().getObject());
}

TEST_F(CreateFromQuery, OrQueryOp) {
    BSONObj query = fromjson("{$or:[{a:1}]}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(fromjson("{a:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, OrQueryIdRepl) {
    BSONObj query = fromjson("{$or:[{_id:1}]}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(fromjson("{_id:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, OrQueryNoExtractOps) {
    BSONObj query = fromjson("{$or:[{a:1}, {b:2}]}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(BSONObj(), doc().getObject());
}

TEST_F(CreateFromQuery, OrQueryNoExtractIdRepl) {
    BSONObj query = fromjson("{$or:[{_id:1}, {_id:2}]}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(BSONObj(), doc().getObject());
}

TEST_F(CreateFromQuery, AndQueryOp) {
    BSONObj query = fromjson("{$and:[{'a.c':1},{'a.b':{$eq:2}}]}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(fromjson("{a:{c:1,b:2}}"), doc().getObject());
}

TEST_F(CreateFromQuery, AndQueryIdRepl) {
    BSONObj query = fromjson("{$and:[{_id:1},{a:{$eq:2}}]}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(fromjson("{_id:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, AllArrayOp) {
    BSONObj query = fromjson("{a:{$all:[1]}}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(fromjson("{a:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, AllArrayIdRepl) {
    BSONObj query = fromjson("{_id:{$all:[1]}, b:2}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(fromjson("{_id:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, ConflictFieldsFailOp) {
    BSONObj query = fromjson("{a:1,'a.b':1}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_NOT_OK(
        driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
}

TEST_F(CreateFromQuery, ConflictFieldsFailSameValueOp) {
    BSONObj query = fromjson("{a:{b:1},'a.b':1}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_NOT_OK(
        driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
}

TEST_F(CreateFromQuery, ConflictWithIdRepl) {
    BSONObj query = fromjson("{_id:1,'_id.a':1}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_NOT_OK(
        driverRepl().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
}

TEST_F(CreateFromQuery, ConflictAndQueryOp) {
    BSONObj query = fromjson("{$and:[{a:{b:1}},{'a.b':{$eq:1}}]}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_NOT_OK(
        driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
}

TEST_F(CreateFromQuery, ConflictAllMultipleValsOp) {
    BSONObj query = fromjson("{a:{$all:[1, 2]}}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_NOT_OK(
        driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
}

TEST_F(CreateFromQuery, NoConflictOrQueryOp) {
    BSONObj query = fromjson("{$or:[{a:{b:1}},{'a.b':{$eq:1}}]}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(BSONObj(), doc().getObject());
}

TEST_F(CreateFromQuery, ImmutableFieldsOp) {
    BSONObj query = fromjson("{$or:[{a:{b:1}},{'a.b':{$eq:1}}]}");
    FieldRef idFieldRef("_id");
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(BSONObj(), doc().getObject());
}

TEST_F(CreateFromQuery, ShardKeyRepl) {
    BSONObj query = fromjson("{a:{$eq:1}}, b:2}");
    std::vector<std::unique_ptr<FieldRef>> immutablePathsVector;
    immutablePathsVector.push_back(std::make_unique<FieldRef>("a"));
    immutablePathsVector.push_back(std::make_unique<FieldRef>("_id"));
    FieldRefSet immutablePaths;
    immutablePaths.fillFrom(immutablePathsVector);
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(fromjson("{a:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, NestedShardKeyRepl) {
    BSONObj query = fromjson("{a:{$eq:1},'b.c':2},d:2}");
    std::vector<std::unique_ptr<FieldRef>> immutablePathsVector;
    immutablePathsVector.push_back(std::make_unique<FieldRef>("a"));
    immutablePathsVector.push_back(std::make_unique<FieldRef>("b.c"));
    immutablePathsVector.push_back(std::make_unique<FieldRef>("_id"));
    FieldRefSet immutablePaths;
    immutablePaths.fillFrom(immutablePathsVector);
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(fromjson("{a:1,b:{c:2}}"), doc().getObject());
}

TEST_F(CreateFromQuery, NestedShardKeyOp) {
    BSONObj query = fromjson("{a:{$eq:1},'b.c':2,d:{$all:[3]}},e:2}");
    std::vector<std::unique_ptr<FieldRef>> immutablePathsVector;
    immutablePathsVector.push_back(std::make_unique<FieldRef>("a"));
    immutablePathsVector.push_back(std::make_unique<FieldRef>("b.c"));
    immutablePathsVector.push_back(std::make_unique<FieldRef>("_id"));
    FieldRefSet immutablePaths;
    immutablePaths.fillFrom(immutablePathsVector);
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(fromjson("{a:1,b:{c:2},d:3}"), doc().getObject());
}

TEST_F(CreateFromQuery, NotFullShardKeyRepl) {
    BSONObj query = fromjson("{a:{$eq:1}, 'b.c':2}, d:2}");
    std::vector<std::unique_ptr<FieldRef>> immutablePathsVector;
    immutablePathsVector.push_back(std::make_unique<FieldRef>("a"));
    immutablePathsVector.push_back(std::make_unique<FieldRef>("b"));
    immutablePathsVector.push_back(std::make_unique<FieldRef>("_id"));
    FieldRefSet immutablePaths;
    immutablePaths.fillFrom(immutablePathsVector);
    ASSERT_NOT_OK(
        driverRepl().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
}

class ModifiedPathsTestFixture : public mongo::unittest::Test {
public:
    void runUpdate(mutablebson::Document* doc,
                   const write_ops::UpdateModification& updateSpec,
                   StringData matchedField = StringData(),
                   std::vector<BSONObj> arrayFilterSpec = {},
                   bool fromOplog = false,
                   UpdateIndexData* indexData = nullptr) {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        _driver = std::make_unique<UpdateDriver>(expCtx);
        std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;

        for (const auto& filter : arrayFilterSpec) {
            auto parsedFilter = assertGet(MatchExpressionParser::parse(filter, expCtx));
            auto expr = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
            ASSERT(expr->getPlaceholder());
            arrayFilters[expr->getPlaceholder().get()] = std::move(expr);
        }

        _driver->setFromOplogApplication(fromOplog);
        _driver->refreshIndexKeys(indexData);
        _driver->parse(updateSpec, arrayFilters);

        const bool validateForStorage = true;
        const FieldRefSet emptyImmutablePaths;
        const bool isInsert = false;
        FieldRefSetWithStorage modifiedPaths;
        ASSERT_OK(_driver->update(
            expCtx->opCtx,
            matchedField,
            doc,
            validateForStorage,
            emptyImmutablePaths,
            isInsert,
            nullptr,
            nullptr,
            (_driver->type() == UpdateDriver::UpdateType::kOperator) ? &modifiedPaths : nullptr));

        _modifiedPaths = modifiedPaths.toString();
    }

    std::unique_ptr<UpdateDriver> _driver;
    std::string _modifiedPaths;
};

TEST_F(ModifiedPathsTestFixture, SetFieldInRoot) {
    BSONObj spec = fromjson("{$set: {a: 1}}");
    mutablebson::Document doc(fromjson("{a: 0}"));
    runUpdate(&doc, makeUpdateMod(spec));
    ASSERT_EQ(_modifiedPaths, "{a}");
}

TEST_F(ModifiedPathsTestFixture, IncFieldInRoot) {
    BSONObj spec = fromjson("{$inc: {a: 1}}");
    mutablebson::Document doc(fromjson("{a: 0}"));
    runUpdate(&doc, makeUpdateMod(spec));
    ASSERT_EQ(_modifiedPaths, "{a}");
}

TEST_F(ModifiedPathsTestFixture, UnsetFieldInRoot) {
    BSONObj spec = fromjson("{$unset: {a: ''}}");
    mutablebson::Document doc(fromjson("{a: 0}"));
    runUpdate(&doc, makeUpdateMod(spec));
    ASSERT_EQ(_modifiedPaths, "{a}");
}

TEST_F(ModifiedPathsTestFixture, UpdateArrayElement) {
    BSONObj spec = fromjson("{$set: {'a.0.b': 1}}");
    mutablebson::Document doc(fromjson("{a: [{b: 0}]}"));
    runUpdate(&doc, makeUpdateMod(spec));
    ASSERT_EQ(_modifiedPaths, "{a.0.b}");
}

TEST_F(ModifiedPathsTestFixture, SetBeyondTheEndOfArrayShouldReturnPathToArray) {
    BSONObj spec = fromjson("{$set: {'a.1.b': 1}}");
    mutablebson::Document doc(fromjson("{a: [{b: 0}]}"));
    runUpdate(&doc, makeUpdateMod(spec));
    ASSERT_EQ(_modifiedPaths, "{a}");
}

TEST_F(ModifiedPathsTestFixture, InsertingAndUpdatingArrayShouldReturnPathToArray) {
    BSONObj spec = fromjson("{$set: {'a.0.b': 1, 'a.1.c': 2}}");
    mutablebson::Document doc(fromjson("{a: [{b: 0}]}"));
    runUpdate(&doc, makeUpdateMod(spec));
    ASSERT_EQ(_modifiedPaths, "{a}");

    spec = fromjson("{$set: {'a.10.b': 1, 'a.1.c': 2}}");
    mutablebson::Document doc2(fromjson("{a: [{b: 0}, {b: 0}]}"));
    runUpdate(&doc2, makeUpdateMod(spec));
    ASSERT_EQ(_modifiedPaths, "{a}");
}

TEST_F(ModifiedPathsTestFixture, UpdateWithPositionalOperator) {
    BSONObj spec = fromjson("{$set: {'a.$': 1}}");
    mutablebson::Document doc(fromjson("{a: [0, 1, 2]}"));
    runUpdate(&doc, makeUpdateMod(spec), "0"_sd);
    ASSERT_EQ(_modifiedPaths, "{a.0}");
}

TEST_F(ModifiedPathsTestFixture, UpdateWithPositionalOperatorToNestedField) {
    BSONObj spec = fromjson("{$set: {'a.$.b': 1}}");
    mutablebson::Document doc(fromjson("{a: [{b: 1}, {b: 2}]}"));
    runUpdate(&doc, makeUpdateMod(spec), "1"_sd);
    ASSERT_EQ(_modifiedPaths, "{a.1.b}");
}

TEST_F(ModifiedPathsTestFixture, ArrayFilterThatMatchesNoElements) {
    BSONObj spec = fromjson("{$set: {'a.$[i]': 1}}");
    BSONObj arrayFilter = fromjson("{i: 0}");
    mutablebson::Document doc(fromjson("{a: [1, 2, 3]}"));
    runUpdate(&doc, makeUpdateMod(spec), ""_sd, {arrayFilter});
    ASSERT_EQ(_modifiedPaths, "{a}");
}


TEST_F(ModifiedPathsTestFixture, ReplaceFullDocumentAlwaysAffectsIndex) {
    BSONObj spec = fromjson("{a: 1, b: 1}}");
    mutablebson::Document doc(fromjson("{a: 0, b: 0}"));
    runUpdate(&doc, makeUpdateMod(spec));
    ASSERT_EQ(_modifiedPaths, "{}");
}


TEST_F(ModifiedPathsTestFixture, PipelineUpdatesAlwaysAffectsIndex) {
    BSONObj spec = fromjson("{$set: {'a.1.b': 1}}");
    mutablebson::Document doc(fromjson("{a: [{b: 0}]}"));
    runUpdate(&doc, std::vector<BSONObj>{spec});
    ASSERT(_driver->modsAffectIndices());
}

TEST_F(ModifiedPathsTestFixture, DeltaUpdateNotAffectingIndex) {
    BSONObj spec = fromjson("{d: {a: false}}");
    mutablebson::Document doc(fromjson("{a: [{b: 0}]}"));
    runUpdate(&doc,
              write_ops::UpdateModification::parseFromV2Delta(
                  spec, write_ops::UpdateModification::DiffOptions{}),
              ""_sd,
              {},
              true /* fromOplog */);
    ASSERT(!_driver->modsAffectIndices());

    UpdateIndexData indexData;
    indexData.addPath(FieldRef("p"));
    runUpdate(&doc,
              write_ops::UpdateModification::parseFromV2Delta(
                  spec, write_ops::UpdateModification::DiffOptions{}),
              ""_sd,
              {},
              true /* fromOplog */,
              &indexData);
    ASSERT(!_driver->modsAffectIndices());
}

TEST_F(ModifiedPathsTestFixture, DeltaUpdateAffectingIndex) {
    BSONObj spec = fromjson("{u: {a: 1}}");
    mutablebson::Document doc(fromjson("{a: [{b: 0}]}"));
    UpdateIndexData indexData;
    indexData.addPath(FieldRef("q"));
    indexData.addPath(FieldRef("a.p"));
    runUpdate(&doc,
              write_ops::UpdateModification::parseFromV2Delta(
                  spec, write_ops::UpdateModification::DiffOptions{}),
              ""_sd,
              {},
              true /* fromOplog */,
              &indexData);
    ASSERT(_driver->modsAffectIndices());
}

}  // namespace
}  // namespace mongo
