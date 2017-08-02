/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_change_notification.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::vector;
using std::string;
using boost::intrusive_ptr;
using repl::OplogEntry;
using repl::OpTypeEnum;
using D = Document;
using V = Value;

static const Timestamp ts(100, 1);
static const repl::OpTime optime(ts, 1);
static const NamespaceString nss("unittests.change_notification");

class ChangeNotificationStageTest : public AggregationContextFixture {
public:
    ChangeNotificationStageTest() : AggregationContextFixture(nss) {}

    void checkTransformation(const OplogEntry& entry, const boost::optional<Document> expectedDoc) {
        const auto spec = fromjson("{$changeNotification: {}}");
        vector<intrusive_ptr<DocumentSource>> result =
            DocumentSourceChangeNotification::createFromBson(spec.firstElement(), getExpCtx());

        auto match = dynamic_cast<DocumentSourceMatch*>(result[0].get());
        ASSERT(match);
        auto mock = DocumentSourceMock::create(D(entry.toBSON()));
        match->setSource(mock.get());

        // Check the oplog entry is transformed correctly.
        auto transform = result.back().get();
        ASSERT(transform);
        ASSERT_EQ(string(transform->getSourceName()), "$changeNotification");
        transform->setSource(match);

        auto next = transform->getNext();
        // Match stage should pass the doc down if expectedDoc is given.
        ASSERT_EQ(next.isAdvanced(), static_cast<bool>(expectedDoc));
        if (expectedDoc) {
            ASSERT_DOCUMENT_EQ(next.releaseDocument(), *expectedDoc);
        }
    }

    OplogEntry createCommand(const BSONObj& oField) {
        return OplogEntry(optime, 1, OpTypeEnum::kCommand, nss.getCommandNS(), oField);
    }
};

TEST_F(ChangeNotificationStageTest, StagesGeneratedCorrectly) {
    const auto spec = fromjson("{$changeNotification: {}}");

    vector<intrusive_ptr<DocumentSource>> result =
        DocumentSourceChangeNotification::createFromBson(spec.firstElement(), getExpCtx());

    ASSERT_EQUALS(result.size(), 2UL);
    ASSERT_TRUE(dynamic_cast<DocumentSourceMatch*>(result[0].get()));
    ASSERT_EQUALS(string(result[0]->getSourceName()), "$changeNotification");
    ASSERT_EQUALS(string(result[1]->getSourceName()), "$changeNotification");

    // TODO: Check explain result.
}

TEST_F(ChangeNotificationStageTest, TransformInsert) {
    OplogEntry insert(optime, 1, OpTypeEnum::kInsert, nss, BSON("_id" << 1 << "x" << 1));
    // Insert
    Document expectedInsert{
        {"_id", D{{"ts", ts}, {"ns", nss.ns()}, {"_id", 1}}},
        {"operationType", "insert"_sd},
        {"ns", D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {"documentKey", D{{"_id", 1}}},
        {"newDocument", D{{"_id", 1}, {"x", 1}}},
    };
    checkTransformation(insert, expectedInsert);
}

TEST_F(ChangeNotificationStageTest, TransformUpdateFields) {
    OplogEntry updateField(
        optime, 1, OpTypeEnum::kUpdate, nss, BSON("$set" << BSON("y" << 1)), BSON("_id" << 1));
    // Update fields
    Document expectedUpdateField{
        {"_id", D{{"ts", ts}, {"ns", nss.ns()}, {"_id", 1}}},
        {"operationType", "update"_sd},
        {"ns", D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {"documentKey", D{{"_id", 1}}},
        {
            "updateDescription", D{{"updatedFields", D{{"y", 1}}}, {"removedFields", vector<V>()}},
        }};
    checkTransformation(updateField, expectedUpdateField);
}

TEST_F(ChangeNotificationStageTest, TransformRemoveFields) {
    OplogEntry removeField(
        optime, 1, OpTypeEnum::kUpdate, nss, BSON("$unset" << BSON("y" << 1)), BSON("_id" << 1));
    // Remove fields
    Document expectedRemoveField{
        {"_id", D{{"ts", ts}, {"ns", nss.ns()}, {"_id", 1}}},
        {"operationType", "update"_sd},
        {"ns", D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {"documentKey", D{{"_id", 1}}},
        {
            "updateDescription", D{{"updatedFields", D{}}, {"removedFields", vector<V>{V("y"_sd)}}},
        }};
    checkTransformation(removeField, expectedRemoveField);
}

TEST_F(ChangeNotificationStageTest, TransformReplace) {
    OplogEntry replace(
        optime, 1, OpTypeEnum::kUpdate, nss, BSON("_id" << 1 << "y" << 1), BSON("_id" << 1));
    // Replace
    Document expectedReplace{
        {"_id", D{{"ts", ts}, {"ns", nss.ns()}, {"_id", 1}}},
        {"operationType", "replace"_sd},
        {"ns", D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {"documentKey", D{{"_id", 1}}},
        {"newDocument", D{{"_id", 1}, {"y", 1}}},
    };
    checkTransformation(replace, expectedReplace);
}

TEST_F(ChangeNotificationStageTest, TransformDelete) {
    OplogEntry deleteEntry(optime, 1, OpTypeEnum::kDelete, nss, BSON("_id" << 1));
    // Delete
    Document expectedDelete{
        {"_id", D{{"ts", ts}, {"ns", nss.ns()}, {"_id", 1}}},
        {"operationType", "delete"_sd},
        {"ns", D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {"documentKey", D{{"_id", 1}}},
    };
    checkTransformation(deleteEntry, expectedDelete);
}

TEST_F(ChangeNotificationStageTest, TransformInvalidate) {
    NamespaceString otherColl("test.bar");

    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()));
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1));
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << otherColl.ns()));

    // Invalidate entry includes $cmd namespace in _id and doesn't have a document id.
    Document expectedInvalidate{
        {"_id", D{{"ts", ts}, {"ns", nss.getCommandNS().ns()}}}, {"operationType", "invalidate"_sd},
    };
    for (auto& entry : {dropColl, dropDB, rename}) {
        checkTransformation(entry, expectedInvalidate);
    }
}

TEST_F(ChangeNotificationStageTest, TransformInvalidateRenameDropTarget) {
    // renameCollection command with dropTarget: true has the namespace of the "from" database.
    NamespaceString otherColl("test.bar");
    OplogEntry rename(optime,
                      1,
                      OpTypeEnum::kCommand,
                      otherColl.getCommandNS(),
                      BSON("renameCollection" << otherColl.ns() << "to" << nss.ns()));
    Document expectedInvalidate{
        {"_id", D{{"ts", ts}, {"ns", otherColl.getCommandNS().ns()}}},
        {"operationType", "invalidate"_sd},
    };
    checkTransformation(rename, expectedInvalidate);
}

TEST_F(ChangeNotificationStageTest, MatchFiltersCreateCollection) {
    auto collSpec =
        D{{"create", "foo"_sd},
          {"idIndex", D{{"v", 2}, {"key", D{{"_id", 1}}}, {"name", "_id_"_sd}, {"ns", nss.ns()}}}};
    OplogEntry createColl = createCommand(collSpec.toBson());
    checkTransformation(createColl, boost::none);
}

TEST_F(ChangeNotificationStageTest, MatchFiltersNoOp) {
    OplogEntry noOp(
        optime, 1, OpTypeEnum::kNoop, NamespaceString(), fromjson("{'msg':'new primary'}"));
    checkTransformation(noOp, boost::none);
}

TEST_F(ChangeNotificationStageTest, MatchFiltersCreateIndex) {
    auto indexSpec = D{{"v", 2}, {"key", D{{"a", 1}}}, {"name", "a_1"_sd}, {"ns", nss.ns()}};
    NamespaceString indexNs(nss.getSystemIndexesCollection());
    OplogEntry createIndex(optime, 1, OpTypeEnum::kInsert, indexNs, indexSpec.toBson());
    checkTransformation(createIndex, boost::none);
}

}  // namespace
}  // namespace mongo
