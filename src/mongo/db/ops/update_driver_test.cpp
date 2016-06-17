/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/ops/update_driver.h"


#include <map>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/update_index_data.h"
#include "mongo/unittest/unittest.h"

namespace {

using mongo::BSONObj;
using mongo::BSONElement;
using mongo::BSONObjIterator;
using mongo::CollatorInterfaceMock;
using mongo::FieldRef;
using mongo::fromjson;
using mongo::mutablebson::Document;
using mongo::OperationContext;
using mongo::OwnedPointerVector;
using mongo::QueryTestServiceContext;
using mongo::ServiceContext;
using mongo::StringData;
using mongo::UpdateDriver;
using mongo::UpdateIndexData;
using mongoutils::str::stream;

TEST(Parse, Normal) {
    UpdateDriver::Options opts;
    UpdateDriver driver(opts);
    ASSERT_OK(driver.parse(fromjson("{$set:{a:1}}")));
    ASSERT_EQUALS(driver.numMods(), 1U);
    ASSERT_FALSE(driver.isDocReplacement());
}

TEST(Parse, MultiMods) {
    UpdateDriver::Options opts;
    UpdateDriver driver(opts);
    ASSERT_OK(driver.parse(fromjson("{$set:{a:1, b:1}}")));
    ASSERT_EQUALS(driver.numMods(), 2U);
    ASSERT_FALSE(driver.isDocReplacement());
}

TEST(Parse, MixingMods) {
    UpdateDriver::Options opts;
    UpdateDriver driver(opts);
    ASSERT_OK(driver.parse(fromjson("{$set:{a:1}, $unset:{b:1}}")));
    ASSERT_EQUALS(driver.numMods(), 2U);
    ASSERT_FALSE(driver.isDocReplacement());
}

TEST(Parse, ObjectReplacment) {
    UpdateDriver::Options opts;
    UpdateDriver driver(opts);
    ASSERT_OK(driver.parse(fromjson("{obj: \"obj replacement\"}")));
    ASSERT_TRUE(driver.isDocReplacement());
}

TEST(Parse, EmptyMod) {
    UpdateDriver::Options opts;
    UpdateDriver driver(opts);
    ASSERT_NOT_OK(driver.parse(fromjson("{$set:{}}")));
}

TEST(Parse, WrongMod) {
    UpdateDriver::Options opts;
    UpdateDriver driver(opts);
    ASSERT_NOT_OK(driver.parse(fromjson("{$xyz:{a:1}}")));
}

TEST(Parse, WrongType) {
    UpdateDriver::Options opts;
    UpdateDriver driver(opts);
    ASSERT_NOT_OK(driver.parse(fromjson("{$set:[{a:1}]}")));
}

TEST(Parse, ModsWithLaterObjReplacement) {
    UpdateDriver::Options opts;
    UpdateDriver driver(opts);
    ASSERT_NOT_OK(driver.parse(fromjson("{$set:{a:1}, obj: \"obj replacement\"}")));
}

TEST(Parse, PushAll) {
    UpdateDriver::Options opts;
    UpdateDriver driver(opts);
    ASSERT_OK(driver.parse(fromjson("{$pushAll:{a:[1,2,3]}}")));
    ASSERT_EQUALS(driver.numMods(), 1U);
    ASSERT_FALSE(driver.isDocReplacement());
}

TEST(Parse, SetOnInsert) {
    UpdateDriver::Options opts;
    UpdateDriver driver(opts);
    ASSERT_OK(driver.parse(fromjson("{$setOnInsert:{a:1}}")));
    ASSERT_EQUALS(driver.numMods(), 1U);
    ASSERT_FALSE(driver.isDocReplacement());
}

TEST(Collator, SetCollationUpdatesModifierInterfaces) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj updateDocument = fromjson("{$max: {a: 'abd'}}");
    UpdateDriver::Options opts;
    UpdateDriver driver(opts);

    ASSERT_OK(driver.parse(updateDocument));
    ASSERT_EQUALS(driver.numMods(), 1U);

    bool modified = false;
    Document doc(fromjson("{a: 'cba'}"));
    driver.setCollator(&collator);
    driver.update(StringData(), &doc, nullptr, nullptr, &modified);

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
        : _driverOps(new UpdateDriver(UpdateDriver::Options())),
          _driverRepl(new UpdateDriver(UpdateDriver::Options())) {
        _driverOps->parse(fromjson("{$set:{'_':1}}"));
        _driverRepl->parse(fromjson("{}"));
        _opCtx = _serviceContext.makeOperationContext();
    }

    Document& doc() {
        return _doc;
    }

    UpdateDriver& driverOps() {
        return *_driverOps;
    }

    UpdateDriver& driverRepl() {
        return *_driverRepl;
    }

    OperationContext* txn() {
        return _opCtx.get();
    }

private:
    QueryTestServiceContext _serviceContext;
    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<UpdateDriver> _driverOps;
    std::unique_ptr<UpdateDriver> _driverRepl;
    Document _doc;
};

// Make name nicer to report
typedef CreateFromQueryFixture CreateFromQuery;

static void assertSameFields(const BSONObj& docA, const BSONObj& docB);

/**
 * Recursively asserts that two BSONElements contain the same data or sub-elements,
 * ignoring element order.
 */
static void assertSameElements(const BSONElement& elA, const BSONElement& elB) {
    if (elA.type() != elB.type() || (!elA.isABSONObj() && !elA.valuesEqual(elB))) {
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
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(query, doc().getObject());
}

TEST_F(CreateFromQuery, BasicOpEq) {
    BSONObj query = fromjson("{a:{$eq:1}}");
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(fromjson("{a:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, BasicOpWithId) {
    BSONObj query = fromjson("{_id:1,a:1,b:2}");
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(query, doc().getObject());
}

TEST_F(CreateFromQuery, BasicRepl) {
    BSONObj query = fromjson("{a:1,b:2}");
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(fromjson("{}"), doc().getObject());
}

TEST_F(CreateFromQuery, BasicReplWithId) {
    BSONObj query = fromjson("{_id:1,a:1,b:2}");
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(fromjson("{_id:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, BasicReplWithIdEq) {
    BSONObj query = fromjson("{_id:{$eq:1},a:1,b:2}");
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(fromjson("{_id:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, NoRootIdOp) {
    BSONObj query = fromjson("{'_id.a':1,'_id.b':2}");
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(fromjson("{_id:{a:1,b:2}}"), doc().getObject());
}

TEST_F(CreateFromQuery, NoRootIdRepl) {
    BSONObj query = fromjson("{'_id.a':1,'_id.b':2}");
    ASSERT_NOT_OK(driverRepl().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
}

TEST_F(CreateFromQuery, NestedSharedRootOp) {
    BSONObj query = fromjson("{'a.c':1,'a.b':{$eq:2}}");
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(fromjson("{a:{c:1,b:2}}"), doc().getObject());
}

TEST_F(CreateFromQuery, OrQueryOp) {
    BSONObj query = fromjson("{$or:[{a:1}]}");
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(fromjson("{a:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, OrQueryIdRepl) {
    BSONObj query = fromjson("{$or:[{_id:1}]}");
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(fromjson("{_id:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, OrQueryNoExtractOps) {
    BSONObj query = fromjson("{$or:[{a:1}, {b:2}]}");
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(BSONObj(), doc().getObject());
}

TEST_F(CreateFromQuery, OrQueryNoExtractIdRepl) {
    BSONObj query = fromjson("{$or:[{_id:1}, {_id:2}]}");
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(BSONObj(), doc().getObject());
}

TEST_F(CreateFromQuery, AndQueryOp) {
    BSONObj query = fromjson("{$and:[{'a.c':1},{'a.b':{$eq:2}}]}");
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(fromjson("{a:{c:1,b:2}}"), doc().getObject());
}

TEST_F(CreateFromQuery, AndQueryIdRepl) {
    BSONObj query = fromjson("{$and:[{_id:1},{a:{$eq:2}}]}");
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(fromjson("{_id:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, AllArrayOp) {
    BSONObj query = fromjson("{a:{$all:[1]}}");
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(fromjson("{a:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, AllArrayIdRepl) {
    BSONObj query = fromjson("{_id:{$all:[1]}, b:2}");
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(fromjson("{_id:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, ConflictFieldsFailOp) {
    BSONObj query = fromjson("{a:1,'a.b':1}");
    ASSERT_NOT_OK(driverOps().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
}

TEST_F(CreateFromQuery, ConflictFieldsFailSameValueOp) {
    BSONObj query = fromjson("{a:{b:1},'a.b':1}");
    ASSERT_NOT_OK(driverOps().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
}

TEST_F(CreateFromQuery, ConflictWithIdRepl) {
    BSONObj query = fromjson("{_id:1,'_id.a':1}");
    ASSERT_NOT_OK(driverRepl().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
}

TEST_F(CreateFromQuery, ConflictAndQueryOp) {
    BSONObj query = fromjson("{$and:[{a:{b:1}},{'a.b':{$eq:1}}]}");
    ASSERT_NOT_OK(driverOps().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
}

TEST_F(CreateFromQuery, ConflictAllMultipleValsOp) {
    BSONObj query = fromjson("{a:{$all:[1, 2]}}");
    ASSERT_NOT_OK(driverOps().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
}

TEST_F(CreateFromQuery, NoConflictOrQueryOp) {
    BSONObj query = fromjson("{$or:[{a:{b:1}},{'a.b':{$eq:1}}]}");
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(BSONObj(), doc().getObject());
}

TEST_F(CreateFromQuery, ImmutableFieldsOp) {
    BSONObj query = fromjson("{$or:[{a:{b:1}},{'a.b':{$eq:1}}]}");
    ASSERT_OK(driverOps().populateDocumentWithQueryFields(txn(), query, NULL, doc()));
    assertSameFields(BSONObj(), doc().getObject());
}

TEST_F(CreateFromQuery, ShardKeyRepl) {
    BSONObj query = fromjson("{a:{$eq:1}}, b:2}");
    OwnedPointerVector<FieldRef> immutablePaths;
    immutablePaths.push_back(new FieldRef("a"));
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(
        txn(), query, &immutablePaths.vector(), doc()));
    assertSameFields(fromjson("{a:1}"), doc().getObject());
}

TEST_F(CreateFromQuery, NestedShardKeyRepl) {
    BSONObj query = fromjson("{a:{$eq:1},'b.c':2},d:2}");
    OwnedPointerVector<FieldRef> immutablePaths;
    immutablePaths.push_back(new FieldRef("a"));
    immutablePaths.push_back(new FieldRef("b.c"));
    ASSERT_OK(driverRepl().populateDocumentWithQueryFields(
        txn(), query, &immutablePaths.vector(), doc()));
    assertSameFields(fromjson("{a:1,b:{c:2}}"), doc().getObject());
}

TEST_F(CreateFromQuery, NestedShardKeyOp) {
    BSONObj query = fromjson("{a:{$eq:1},'b.c':2,d:{$all:[3]}},e:2}");
    OwnedPointerVector<FieldRef> immutablePaths;
    immutablePaths.push_back(new FieldRef("a"));
    immutablePaths.push_back(new FieldRef("b.c"));
    ASSERT_OK(
        driverOps().populateDocumentWithQueryFields(txn(), query, &immutablePaths.vector(), doc()));
    assertSameFields(fromjson("{a:1,b:{c:2},d:3}"), doc().getObject());
}

TEST_F(CreateFromQuery, NotFullShardKeyRepl) {
    BSONObj query = fromjson("{a:{$eq:1}, 'b.c':2}, d:2}");
    OwnedPointerVector<FieldRef> immutablePaths;
    immutablePaths.push_back(new FieldRef("a"));
    immutablePaths.push_back(new FieldRef("b"));
    ASSERT_NOT_OK(driverRepl().populateDocumentWithQueryFields(
        txn(), query, &immutablePaths.vector(), doc()));
}

}  // unnamed namespace
