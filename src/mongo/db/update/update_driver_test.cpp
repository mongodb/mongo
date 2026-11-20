// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/update_driver.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/db/update_index_data.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

using unittest::assertGet;

write_ops::UpdateModification makeUpdateMod(const BSONObj& bson) {
    return write_ops::UpdateModification::parseFromClassicUpdate(bson);
}

TEST(Parse, Normal) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_DOES_NOT_THROW(driver.parse(makeUpdateMod(fromjson("{$set:{a:1}}")), arrayFilters));
    ASSERT_FALSE(driver.type() == UpdateDriver::UpdateType::kReplacement);
}

TEST(Parse, MultiMods) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_DOES_NOT_THROW(driver.parse(makeUpdateMod(fromjson("{$set:{a:1, b:1}}")), arrayFilters));
    ASSERT_FALSE(driver.type() == UpdateDriver::UpdateType::kReplacement);
}

TEST(Parse, MixingMods) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_DOES_NOT_THROW(
        driver.parse(makeUpdateMod(fromjson("{$set:{a:1}, $unset:{b:1}}")), arrayFilters));
    ASSERT_FALSE(driver.type() == UpdateDriver::UpdateType::kReplacement);
}

TEST(Parse, ObjectReplacment) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_DOES_NOT_THROW(
        driver.parse(makeUpdateMod(fromjson("{obj: \"obj replacement\"}")), arrayFilters));
    ASSERT_TRUE(driver.type() == UpdateDriver::UpdateType::kReplacement);
}

TEST(Parse, ParseUpdateWithPipeline) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto updateObj = BSON("u" << BSON_ARRAY(BSON("$addFields" << BSON("a" << 1))));
    ASSERT_DOES_NOT_THROW(driver.parse(updateObj["u"], arrayFilters));
    ASSERT_TRUE(driver.type() == UpdateDriver::UpdateType::kPipeline);
}

TEST(Parse, ParseUpdateWithPipelineAndVariables) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    const auto variables = BSON("var1" << 1 << "var2"
                                       << "foo");
    auto updateObj = BSON("u" << BSON_ARRAY(BSON("$set" << BSON("a" << "$$var1"
                                                                    << "b"
                                                                    << "$$var2"))));
    ASSERT_DOES_NOT_THROW(driver.parse(updateObj["u"], arrayFilters, variables));
    ASSERT_TRUE(driver.type() == UpdateDriver::UpdateType::kPipeline);
}

TEST(Parse, EmptyMod) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    // Verifies that {$set: {}} is accepted.
    ASSERT_DOES_NOT_THROW(driver.parse(makeUpdateMod(fromjson("{$set: {}}")), arrayFilters));
}

TEST(Parse, WrongMod) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_THROWS_CODE_AND_WHAT(driver.parse(makeUpdateMod(fromjson("{$xyz:{a:1}}")), arrayFilters),
                                AssertionException,
                                ErrorCodes::FailedToParse,
                                "Unknown modifier: $xyz. Expected a valid update modifier or "
                                "pipeline-style update specified as an array");
}

TEST(Parse, WrongType) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
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
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
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
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_DOES_NOT_THROW(
        driver.parse(makeUpdateMod(fromjson("{$setOnInsert:{a:1}}")), arrayFilters));
    ASSERT_FALSE(driver.type() == UpdateDriver::UpdateType::kReplacement);
}

TEST(Parse, V1OplogUpdatesOnNonOplogApplicationPath) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    // Oplog updates cannot be applied on the fromOplogApplication path
    driver.setFromOplogApplication(false);
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_THROWS_CODE_AND_WHAT(
        driver.parse(makeUpdateMod(fromjson("{$v: 1, $set: {a:1}}")), arrayFilters),
        AssertionException,
        ErrorCodes::FailedToParse,
        "The $v update field is only recognized internally");
}

TEST(Parse, V2OplogUpdatesOnNonOplogApplicationPath) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    // Oplog updates cannot be applied on the fromOplogApplication path
    driver.setFromOplogApplication(false);
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_THROWS_CODE_AND_WHAT(
        driver.parse(makeUpdateMod(fromjson("{$v: 2, diff: {i: {a:1}}}")), arrayFilters),
        AssertionException,
        ErrorCodes::FailedToParse,
        "The $v update field is only recognized internally");
}

TEST(Parse, ExplicitV1OplogEntry) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    // Oplog updates can only be applied on the fromOplogApplication path
    driver.setFromOplogApplication(true);
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_DOES_NOT_THROW(
        driver.parse(makeUpdateMod(fromjson("{$v: 1, $set: {a:1}}")), arrayFilters));
    ASSERT_TRUE(driver.type() == UpdateDriver::UpdateType::kOperator);
}

TEST(Parse, ImplicitV1OplogEntry) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    // Oplog updates can only be applied on the fromOplogApplication path
    driver.setFromOplogApplication(true);
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_DOES_NOT_THROW(driver.parse(makeUpdateMod(fromjson("{$set: {a:1}}")), arrayFilters));
    ASSERT_TRUE(driver.type() == UpdateDriver::UpdateType::kOperator);
}

TEST(Parse, V1WithDuplicateVersionField) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UpdateDriver driver(expCtx);
    // Oplog updates can only be applied on the fromOplogApplication path
    driver.setFromOplogApplication(true);
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    ASSERT_THROWS_CODE_AND_WHAT(
        driver.parse(makeUpdateMod(fromjson("{$v: 1, $set: {a:1}, $v: 1}")), arrayFilters),
        AssertionException,
        ErrorCodes::BadValue,
        "Duplicate $v in oplog update document");
}

TEST(Collator, SetCollationUpdatesModifierInterfaces) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CollatorInterfaceMock reverseStringCollator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj updateDocument = fromjson("{$max: {a: 'abd'}}");
    UpdateDriver driver(expCtx);
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;

    ASSERT_DOES_NOT_THROW(driver.parse(makeUpdateMod(updateDocument), arrayFilters));

    const bool validateForStorage = true;
    const FieldRefSet emptyImmutablePaths;
    const bool isInsert = false;
    bool modified = false;
    mutablebson::Document doc(fromjson("{a: 'cba'}"));
    driver.setCollator(&reverseStringCollator);
    ASSERT_OK(driver.update(expCtx->getOperationContext(),
                            std::string_view(),
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

class CreateFromQueryFixture : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _opCtx = makeOperationContext();
        _driverOps = std::make_unique<UpdateDriver>(
            ExpressionContextBuilder{}
                .opCtx(_opCtx.get())
                .ns(NamespaceString::createNamespaceString_forTest("foo"))
                .build());
        _driverRepl = std::make_unique<UpdateDriver>(
            ExpressionContextBuilder{}
                .opCtx(_opCtx.get())
                .ns(NamespaceString::createNamespaceString_forTest("foo"))
                .build());
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
    ServiceContext::UniqueOperationContext _opCtx;

    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> _arrayFilters;
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
                                 &simpleStringDataComparator);
    if (elA.type() != elB.type() || (!elA.isABSONObj() && eltCmp.evaluate(elA != elB))) {
        FAIL(std::string(str::stream() << "element " << elA << " not equal to " << elB));
    } else if (elA.type() == BSONType::array) {
        std::vector<BSONElement> elsA = elA.Array();
        std::vector<BSONElement> elsB = elB.Array();
        if (elsA.size() != elsB.size())
            FAIL(std::string(str::stream() << "element " << elA << " not equal to " << elB));

        std::vector<BSONElement>::iterator arrItA = elsA.begin();
        std::vector<BSONElement>::iterator arrItB = elsB.begin();
        for (; arrItA != elsA.end(); ++arrItA, ++arrItB) {
            assertSameElements(*arrItA, *arrItB);
        }
    } else if (elA.type() == BSONType::object) {
        assertSameFields(elA.Obj(), elB.Obj());
    }
}

/**
 * Recursively asserts that two BSONObjects contain the same elements,
 * ignoring element order.
 */
static void assertSameFields(const BSONObj& docA, const BSONObj& docB) {
    if (docA.nFields() != docB.nFields())
        FAIL(std::string(str::stream()
                         << "document " << docA << " has different fields than " << docB));

    std::map<std::string_view, BSONElement> docAMap;
    BSONObjIterator itA(docA);
    while (itA.more()) {
        BSONElement elA = itA.next();
        docAMap.insert(std::make_pair(elA.fieldNameStringData(), elA));
    }

    BSONObjIterator itB(docB);
    while (itB.more()) {
        BSONElement elB = itB.next();

        std::map<std::string_view, BSONElement>::iterator seenIt =
            docAMap.find(elB.fieldNameStringData());
        if (seenIt == docAMap.end())
            FAIL(std::string(str::stream() << "element " << elB << " not found in " << docA));

        BSONElement elA = seenIt->second;
        assertSameElements(elA, elB);
    }
}

TEST_F(CreateFromQuery, PaddingLargeArrayThrowsWhenOperationIsKilled) {
    BSONObj query = fromjson("{'a.5000': 1}");
    FieldRefSet immutablePaths;
    ASSERT_OK(doc().root().pushBack(doc().makeElementArray("a")));
    opCtx()->markKilled();
    ASSERT_THROWS_CODE(
        driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()),
        DBException,
        ErrorCodes::Interrupted);
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
    BSONObj query = fromjson("{a:{$eq:1}, b:2}");
    std::vector<std::unique_ptr<FieldRef>> immutablePathsVector;
    immutablePathsVector.push_back(std::make_unique<FieldRef>("a"));
    immutablePathsVector.push_back(std::make_unique<FieldRef>("_id"));
    FieldRefSet immutablePaths;
    immutablePaths.fillFrom(immutablePathsVector);
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(fromjson("{a:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, NestedShardKeyRepl) {
    BSONObj query = fromjson("{a:{$eq:1},'b.c':2, d:2}");
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
    BSONObj query = fromjson("{a:{$eq:1},'b.c':2,d:{$all:[3]},e:2}");
    std::vector<std::unique_ptr<FieldRef>> immutablePathsVector;
    immutablePathsVector.push_back(std::make_unique<FieldRef>("a"));
    immutablePathsVector.push_back(std::make_unique<FieldRef>("b.c"));
    immutablePathsVector.push_back(std::make_unique<FieldRef>("_id"));
    FieldRefSet immutablePaths;
    immutablePaths.fillFrom(immutablePathsVector);
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
    assertSameFields(fromjson("{a:1,b:{c:2},d:3,e:2}"), doc().getObject());
}

TEST_F(CreateFromQuery, NotFullShardKeyRepl) {
    BSONObj query = fromjson("{a:{$eq:1}, 'b.c':2, d:2}");
    std::vector<std::unique_ptr<FieldRef>> immutablePathsVector;
    immutablePathsVector.push_back(std::make_unique<FieldRef>("a"));
    immutablePathsVector.push_back(std::make_unique<FieldRef>("b"));
    immutablePathsVector.push_back(std::make_unique<FieldRef>("_id"));
    FieldRefSet immutablePaths;
    immutablePaths.fillFrom(immutablePathsVector);
    ASSERT_NOT_OK(
        driverRepl().populateDocumentWithQueryFields(opCtx(), query, immutablePaths, doc()));
}

class ModifiedPathsTestFixture : public unittest::Test {
public:
    void runUpdate(mutablebson::Document* doc,
                   const write_ops::UpdateModification& updateSpec,
                   std::string_view matchedField = std::string_view(),
                   std::vector<BSONObj> arrayFilterSpec = {},
                   bool fromOplog = false) {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        _driver = std::make_unique<UpdateDriver>(expCtx);
        std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;

        for (const auto& filter : arrayFilterSpec) {
            auto parsedFilter = assertGet(MatchExpressionParser::parse(filter, expCtx));
            auto expr = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
            ASSERT(expr->getPlaceholder());
            arrayFilters[expr->getPlaceholder().value()] = std::move(expr);
        }

        _driver->setFromOplogApplication(fromOplog);
        _driver->parse(updateSpec, arrayFilters);

        const bool validateForStorage = true;
        const FieldRefSet emptyImmutablePaths;
        const bool isInsert = false;
        FieldRefSetWithStorage modifiedPaths;
        ASSERT_OK(_driver->update(
            expCtx->getOperationContext(),
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
    runUpdate(&doc, makeUpdateMod(spec), "0"sv);
    ASSERT_EQ(_modifiedPaths, "{a.0}");
}

TEST_F(ModifiedPathsTestFixture, UpdateWithPositionalOperatorToNestedField) {
    BSONObj spec = fromjson("{$set: {'a.$.b': 1}}");
    mutablebson::Document doc(fromjson("{a: [{b: 1}, {b: 2}]}"));
    runUpdate(&doc, makeUpdateMod(spec), "1"sv);
    ASSERT_EQ(_modifiedPaths, "{a.1.b}");
}

TEST_F(ModifiedPathsTestFixture, ArrayFilterThatMatchesNoElements) {
    BSONObj spec = fromjson("{$set: {'a.$[i]': 1}}");
    BSONObj arrayFilter = fromjson("{i: 0}");
    mutablebson::Document doc(fromjson("{a: [1, 2, 3]}"));
    runUpdate(&doc, makeUpdateMod(spec), ""sv, {arrayFilter});
    ASSERT_EQ(_modifiedPaths, "{a}");
}


TEST_F(ModifiedPathsTestFixture, ReplaceFullDocumentAlwaysAffectsIndex) {
    BSONObj spec = fromjson("{a: 1, b: 1}");
    mutablebson::Document doc(fromjson("{a: 0, b: 0}"));
    runUpdate(&doc, makeUpdateMod(spec));
    ASSERT_EQ(_modifiedPaths, "{}");
}

TEST_F(ModifiedPathsTestFixture, NeedsMatchDetailsIsTrueForPositionalUpdate) {
    BSONObj spec = fromjson("{$set: {'a.$': 1}}");
    mutablebson::Document doc(fromjson("{a: [0, 1, 2]}"));
    runUpdate(&doc, makeUpdateMod(spec), "0"sv);
    ASSERT_EQ(true, _driver->needMatchDetails());
}

TEST_F(ModifiedPathsTestFixture, NeedsMatchDetailsIsFalseForNonPositionalUpdate) {
    BSONObj spec = fromjson("{$set: {a: 1}}");
    mutablebson::Document doc(fromjson("{a: 0}"));
    runUpdate(&doc, makeUpdateMod(spec));
    ASSERT_EQ(false, _driver->needMatchDetails());
}

//
// ChildLookupCache must not change what an update does. Each test runs an update through
// UpdateDriver with the cache off and with the lowest threshold. Documents and oplog entries are
// compared byte for byte; the other results and error codes are compared by value.
//

constexpr auto kCacheThresholdParam = "internalUpdateObjectNodeChildLookupCacheThreshold"sv;

// A threshold of 1 makes every eligible object node with a static child use the cache.
constexpr int kCacheOff = 0;
constexpr int kCacheMostAggressive = 1;

struct DiffUpdateCase {
    BSONObj update;
    std::vector<BSONObj> arrayFilters = {};
    std::vector<std::string> indexedPaths = {};
};

struct DiffUpdateOutcome {
    bool threw = false;
    int errorCode = 0;
    std::vector<BSONObj> docs;
    std::vector<BSONObj> oplogEntries;
    std::vector<bool> modified;
    std::vector<std::string> modifiedPaths;
    std::vector<bool> indexesAffected;
};

bool indexesAffectedByDiff(const DocumentUpdateRecord& record,
                           const std::vector<std::string>& indexedPaths) {
    if (indexedPaths.empty() || !record.diff) {
        return false;
    }
    UpdateIndexData indexData;
    for (const auto& path : indexedPaths) {
        indexData.addPath(FieldRef(path));
    }
    doc_diff::IndexUpdateIdentifier identifier{1 /*numIndexes*/};
    identifier.addIndex(/*indexCounter=*/0, indexData);
    return identifier.determineAffectedIndexes(*record.diff).any();
}

// Parses 'testCase' once and applies it to each input in turn, as a multi-update does. Stops at the
// first update that throws. Fatal gtest assertions need a void function, hence the out parameter.
void runDiffUpdateCase(int threshold,
                       const DiffUpdateCase& testCase,
                       const std::vector<std::pair<BSONObj, std::string>>& inputs,
                       DiffUpdateOutcome* out) {
    unittest::ServerParameterGuard cacheThreshold(kCacheThresholdParam, threshold);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    for (const auto& filter : testCase.arrayFilters) {
        auto parsedFilter = assertGet(MatchExpressionParser::parse(filter, expCtx));
        auto expr = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
        ASSERT(expr->getPlaceholder());
        arrayFilters[expr->getPlaceholder().value()] = std::move(expr);
    }

    UpdateDriver driver(expCtx);
    driver.parse(makeUpdateMod(testCase.update), arrayFilters, boost::none, /*multi=*/true);

    DiffUpdateOutcome outcome;
    const FieldRefSet immutablePaths;
    for (const auto& [input, matchedField] : inputs) {
        mutablebson::Document doc(input);
        DocumentUpdateRecord record;
        bool docWasModified = false;
        FieldRefSetWithStorage modifiedPaths;
        try {
            uassertStatusOK(driver.update(expCtx->getOperationContext(),
                                          matchedField,
                                          &doc,
                                          /*validateForStorage=*/true,
                                          immutablePaths,
                                          /*isInsert=*/false,
                                          &record,
                                          &docWasModified,
                                          &modifiedPaths));
        } catch (const DBException& ex) {
            outcome.threw = true;
            outcome.errorCode = ex.code();
            break;
        }
        outcome.docs.push_back(doc.getObject());
        outcome.oplogEntries.push_back(record.oplogEntry.getOwned());
        outcome.modified.push_back(docWasModified);
        outcome.modifiedPaths.push_back(modifiedPaths.toString());
        outcome.indexesAffected.push_back(indexesAffectedByDiff(record, testCase.indexedPaths));
    }
    *out = std::move(outcome);
}

// Runs 'testCase' with the cache off and on and checks that the results are identical. The
// cache-off result is stored in 'baseline' if given.
void assertSameWithAndWithoutCache(const DiffUpdateCase& testCase,
                                   const std::vector<std::pair<BSONObj, std::string>>& inputs,
                                   DiffUpdateOutcome* baseline = nullptr) {
    DiffUpdateOutcome off, on;
    ASSERT_NO_FATAL_FAILURE(runDiffUpdateCase(kCacheOff, testCase, inputs, &off));
    ASSERT_NO_FATAL_FAILURE(runDiffUpdateCase(kCacheMostAggressive, testCase, inputs, &on));

    ASSERT_EQ(off.threw, on.threw) << testCase.update;
    ASSERT_EQ(off.errorCode, on.errorCode) << testCase.update;
    ASSERT_EQ(off.docs.size(), on.docs.size()) << testCase.update;
    for (size_t i = 0; i < off.docs.size(); ++i) {
        // Binary comparison also catches reordered and duplicated fields.
        ASSERT_BSONOBJ_BINARY_EQ(off.docs[i], on.docs[i]);
        ASSERT_BSONOBJ_BINARY_EQ(off.oplogEntries[i], on.oplogEntries[i]);
        ASSERT_EQ(off.modified[i], on.modified[i]) << testCase.update;
        ASSERT_EQ(off.modifiedPaths[i], on.modifiedPaths[i]) << testCase.update;
        ASSERT_EQ(off.indexesAffected[i], on.indexesAffected[i]) << testCase.update;
    }
    if (baseline) {
        *baseline = std::move(off);
    }
}

// Returns {<prefix>0: base, <prefix>1: base + 1, ...}.
BSONObj makeWideObject(const std::string& prefix, int numFields, int base = 0) {
    BSONObjBuilder bob;
    for (int i = 0; i < numFields; ++i) {
        bob.append(prefix + std::to_string(i), base + i);
    }
    return bob.obj();
}

TEST(UpdateDriverChildLookupCacheTest, MixedOperatorsOnWideObject) {
    static constexpr int kNumFields = 40;
    BSONObjBuilder setBob, incBob, unsetBob, maxBob;
    for (int i = 0; i < kNumFields; i += 4) {
        setBob.append("w.f" + std::to_string(i), "set");
        incBob.append("w.f" + std::to_string(i + 1), 10);
        unsetBob.append("w.f" + std::to_string(i + 2), "");
        maxBob.append("w.f" + std::to_string(i + 3), 1000);
    }
    setBob.append("w.created", 1);
    BSONObj update =
        BSON("$set" << setBob.obj() << "$inc" << incBob.obj() << "$unset" << unsetBob.obj()
                    << "$max" << maxBob.obj() << "$rename" << BSON("w.renameSrc" << "w.renameDst"));

    BSONObjBuilder docBob;
    {
        BSONObjBuilder w(docBob.subobjStart("w"));
        w.appendElements(makeWideObject("f", kNumFields));
        w.append("renameSrc", 7);
    }

    DiffUpdateOutcome outcome;
    ASSERT_NO_FATAL_FAILURE(
        assertSameWithAndWithoutCache({update}, {{docBob.obj(), ""}}, &outcome));
    ASSERT_FALSE(outcome.threw);
    ASSERT_EQ(outcome.docs.size(), 1U);
    ASSERT_TRUE(outcome.modified[0]);
}

TEST(UpdateDriverChildLookupCacheTest, RenameWithinAndAcrossWideObjects) {
    // Renames within one wide object and from one wide object into another.
    BSONObjBuilder renameBob;
    for (int i = 0; i < 10; ++i) {
        renameBob.append("a.src" + std::to_string(i), "a.dst" + std::to_string(i));
        renameBob.append("b.x" + std::to_string(i), "c.y" + std::to_string(i));
    }
    BSONObj update = BSON("$rename" << renameBob.obj() << "$set" << BSON("a.keep" << 1));

    BSONObjBuilder docBob;
    docBob.append("a", makeWideObject("src", 10));
    docBob.append("b", makeWideObject("x", 10, 100));
    docBob.append("c", makeWideObject("other", 5));

    DiffUpdateOutcome outcome;
    ASSERT_NO_FATAL_FAILURE(
        assertSameWithAndWithoutCache({update}, {{docBob.obj(), ""}}, &outcome));
    ASSERT_FALSE(outcome.threw);
}

TEST(UpdateDriverChildLookupCacheTest, RenameSourceRemovedBeforeLookupWithDuplicateName) {
    // Each destination sorts before its source, so the source is removed before its placeholder is
    // looked up, and the cache still holds the removed element. Every source name appears twice;
    // only the first copy is renamed.
    BSONObjBuilder renameBob;
    BSONObjBuilder docBob;
    {
        BSONObjBuilder a(docBob.subobjStart("a"));
        for (int i = 0; i < 5; ++i) {
            const auto suffix = std::to_string(i);
            renameBob.append("a.z" + suffix, "a.b" + suffix);
            a.append("z" + suffix, "first");
            a.append("z" + suffix, "second");
        }
    }
    BSONObj update = BSON("$rename" << renameBob.obj());

    DiffUpdateOutcome outcome;
    ASSERT_NO_FATAL_FAILURE(
        assertSameWithAndWithoutCache({update}, {{docBob.obj(), ""}}, &outcome));
    ASSERT_FALSE(outcome.threw);
    ASSERT_EQ(outcome.docs.size(), 1U);
    ASSERT_TRUE(outcome.modified[0]);

    const auto a = outcome.docs[0]["a"].Obj();
    for (int i = 0; i < 5; ++i) {
        const auto suffix = std::to_string(i);
        ASSERT_EQ(a["b" + suffix].String(), "first") << a;
        ASSERT_EQ(a["z" + suffix].String(), "second") << a;
    }
}

TEST(UpdateDriverChildLookupCacheTest, NestedWideObjectsAndMissingParents) {
    // An existing nested object, a missing top-level parent and a missing intermediate level.
    BSONObjBuilder setBob;
    for (int i = 0; i < 20; ++i) {
        setBob.append("p.q.f" + std::to_string(i), i * 2);
        setBob.append("missing.f" + std::to_string(i), i);
        setBob.append("p.newLevel.f" + std::to_string(i), i);
    }
    BSONObj update = BSON("$set" << setBob.obj());

    BSONObjBuilder docBob;
    {
        BSONObjBuilder p(docBob.subobjStart("p"));
        p.append("q", makeWideObject("f", 30));
    }
    // Same names as the targets under "missing".
    docBob.appendElements(makeWideObject("f", 20));

    DiffUpdateOutcome outcome;
    ASSERT_NO_FATAL_FAILURE(
        assertSameWithAndWithoutCache({update}, {{docBob.obj(), ""}}, &outcome));
    ASSERT_FALSE(outcome.threw);
}

TEST(UpdateDriverChildLookupCacheTest, WideObjectsInsideArrayWithArrayFilters) {
    // The array node never uses the cache, but the object node applied to each element does.
    BSONObjBuilder setBob;
    for (int i = 0; i < 15; ++i) {
        setBob.append("arr.$[e].f" + std::to_string(i), "filtered");
    }
    BSONObjBuilder incBob;
    for (int i = 0; i < 15; ++i) {
        incBob.append("arr.$[].g" + std::to_string(i), 1);
    }
    BSONObj update = BSON("$set" << setBob.obj() << "$inc" << incBob.obj());

    BSONArrayBuilder arr;
    for (int k = 0; k < 4; ++k) {
        BSONObjBuilder elem;
        elem.append("kind", k % 2);
        elem.appendElements(makeWideObject("f", 15));
        elem.appendElements(makeWideObject("g", 15, k * 100));
        arr.append(elem.obj());
    }

    DiffUpdateOutcome outcome;
    ASSERT_NO_FATAL_FAILURE(assertSameWithAndWithoutCache(
        {update, {fromjson("{'e.kind': 1}")}}, {{BSON("arr" << arr.arr()), ""}}, &outcome));
    ASSERT_FALSE(outcome.threw);
    ASSERT_TRUE(outcome.modified[0]);
}

TEST(UpdateDriverChildLookupCacheTest, PositionalOnRealArray) {
    BSONObjBuilder setBob;
    for (int i = 0; i < 12; ++i) {
        setBob.append("arr.$.f" + std::to_string(i), "pos");
    }
    setBob.append("arr.0.f0", "static0");
    BSONObj update = BSON("$set" << setBob.obj());

    BSONArrayBuilder arr;
    for (int k = 0; k < 12; ++k) {
        arr.append(makeWideObject("f", 12, k * 10));
    }
    BSONObj doc = BSON("arr" << arr.arr());
    // Digit-only names are ordered numerically, so try one and two digit indexes.
    for (const char* matched : {"3", "10", "11"}) {
        DiffUpdateOutcome outcome;
        ASSERT_NO_FATAL_FAILURE(
            assertSameWithAndWithoutCache({update}, {{doc, matched}}, &outcome));
        ASSERT_FALSE(outcome.threw) << matched;
    }
}

TEST(UpdateDriverChildLookupCacheTest, AffectedIndexesMatch) {
    BSONObjBuilder setBob;
    for (int i = 0; i < 30; ++i) {
        setBob.append("w.f" + std::to_string(i), "v");
    }
    BSONObj update = BSON("$set" << setBob.obj());
    BSONObj doc = BSON("w" << makeWideObject("f", 30));

    DiffUpdateOutcome touched;
    ASSERT_NO_FATAL_FAILURE(
        assertSameWithAndWithoutCache({update, {}, {"w.f7"}}, {{doc, ""}}, &touched));
    ASSERT_EQ(touched.indexesAffected.size(), 1U);
    ASSERT_TRUE(touched.indexesAffected[0]);

    DiffUpdateOutcome untouched;
    ASSERT_NO_FATAL_FAILURE(
        assertSameWithAndWithoutCache({update, {}, {"w.notUpdated"}}, {{doc, ""}}, &untouched));
    ASSERT_EQ(untouched.indexesAffected.size(), 1U);
    ASSERT_FALSE(untouched.indexesAffected[0]);
}

TEST(UpdateDriverChildLookupCacheTest, FailingUpdatesReportSameErrorCode) {
    // Each update fails after the "f*" fields under 'w', which sort first, have been updated.
    BSONObjBuilder docBob;
    {
        BSONObjBuilder w(docBob.subobjStart("w"));
        w.appendElements(makeWideObject("f", 30));
        w.append("txt", "not a number");
    }
    docBob.append("arr", BSON_ARRAY(1 << 2));
    BSONObj doc = docBob.obj();

    BSONObjBuilder siblingsBob;
    for (int i = 0; i < 20; ++i) {
        siblingsBob.append("w.f" + std::to_string(i), 1000 + i);
    }
    BSONObj siblings = siblingsBob.obj();

    BSONObjBuilder throughScalarBob;
    throughScalarBob.appendElements(siblings);
    throughScalarBob.append("w.f25.x", 1);

    struct FailCase {
        BSONObj update;
        ErrorCodes::Error expected;
    };
    const std::vector<FailCase> cases = {
        {BSON("$set" << siblings << "$inc" << BSON("w.txt" << 1)), ErrorCodes::TypeMismatch},
        // "w.f25" is an int.
        {BSON("$set" << throughScalarBob.obj()), ErrorCodes::PathNotViable},
        {BSON("$set" << siblings << "$rename" << BSON("arr.0" << "w.zzz")), ErrorCodes::BadValue},
    };

    for (const auto& c : cases) {
        DiffUpdateOutcome outcome;
        ASSERT_NO_FATAL_FAILURE(assertSameWithAndWithoutCache({c.update}, {{doc, ""}}, &outcome));
        ASSERT_TRUE(outcome.threw) << c.update;
        ASSERT_EQ(outcome.errorCode, static_cast<int>(c.expected)) << c.update;
    }
}

TEST(UpdateDriverChildLookupCacheTest, NoopUpdateProducesNoOplogEntry) {
    BSONObj doc = BSON("w" << makeWideObject("f", 30));
    BSONObjBuilder setBob;
    for (int i = 0; i < 30; ++i) {
        setBob.append("w.f" + std::to_string(i), i);
    }

    DiffUpdateOutcome outcome;
    ASSERT_NO_FATAL_FAILURE(
        assertSameWithAndWithoutCache({BSON("$set" << setBob.obj())}, {{doc, ""}}, &outcome));
    ASSERT_EQ(outcome.docs.size(), 1U);
    ASSERT_FALSE(outcome.modified[0]);
    // UpdateDriver leaves the oplog entry empty for a no-op.
    ASSERT_TRUE(outcome.oplogEntries[0].isEmpty());
}

TEST(UpdateDriverChildLookupCacheTest, OneExecutorAppliedToManyDocuments) {
    // One parsed update applied to documents of different shapes, as in a multi-update.
    BSONObjBuilder setBob;
    for (int i = 0; i < 25; ++i) {
        setBob.append("w.f" + std::to_string(i), "new");
    }
    BSONObj update = BSON("$set" << setBob.obj() << "$inc" << BSON("w.counter" << 1));

    BSONObjBuilder dupBob;
    dupBob.append("f0", "first");
    dupBob.append("f0", "second");
    dupBob.appendElements(makeWideObject("f", 10, 500));

    const std::vector<std::pair<BSONObj, std::string>> inputs = {
        {BSON("w" << makeWideObject("f", 25)), ""},
        {BSON("w" << makeWideObject("f", 10)), ""},
        {BSON("w" << BSONObj()), ""},
        {BSON("other" << 1), ""},
        {BSON("w" << makeWideObject("f", 200)), ""},
        {BSON("w" << dupBob.obj()), ""},
        {BSON("w" << makeWideObject("f", 25)), ""},
    };

    DiffUpdateOutcome outcome;
    ASSERT_NO_FATAL_FAILURE(assertSameWithAndWithoutCache({update}, inputs, &outcome));
    ASSERT_FALSE(outcome.threw);
    ASSERT_EQ(outcome.docs.size(), inputs.size());
}

}  // namespace
}  // namespace mongo
