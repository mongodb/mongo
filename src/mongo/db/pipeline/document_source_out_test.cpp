/**
 * Copyright 2018 (c) 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_value_test_util.h"

namespace mongo {
namespace {

using boost::intrusive_ptr;

StringData kModeFieldName = DocumentSourceOutSpec::kModeFieldName;
StringData kUniqueKeyFieldName = DocumentSourceOutSpec::kUniqueKeyFieldName;
StringData kDefaultMode = WriteMode_serializer(WriteModeEnum::kModeReplaceCollection);
StringData kInsertDocumentsMode = WriteMode_serializer(WriteModeEnum::kModeInsertDocuments);

/**
 * For the purpsoses of this test, assume every collection is unsharded. Stages may ask this during
 * setup. For example, to compute its constraints, the $out stage needs to know if the output
 * collection is sharded.
 */
class MongoProcessInterfaceForTest : public StubMongoProcessInterface {
public:
    bool isSharded(OperationContext* opCtx, const NamespaceString& ns) override {
        return false;
    }

    /**
     * For the purposes of these tests, pretend each collection is unsharded and has a document key
     * of just "_id".
     */
    std::pair<std::vector<FieldPath>, bool> collectDocumentKeyFields(
        OperationContext* opCtx, NamespaceStringOrUUID nssOrUUID) const override {
        return {{"_id"}, false};
    }
};

class DocumentSourceOutTest : public AggregationContextFixture {
public:
    DocumentSourceOutTest() : AggregationContextFixture() {
        getExpCtx()->mongoProcessInterface = std::make_shared<MongoProcessInterfaceForTest>();
    }

    intrusive_ptr<DocumentSourceOut> createOutStage(BSONObj spec) {
        auto specElem = spec.firstElement();
        intrusive_ptr<DocumentSourceOut> outStage = dynamic_cast<DocumentSourceOut*>(
            DocumentSourceOut::createFromBson(specElem, getExpCtx()).get());
        ASSERT_TRUE(outStage);
        return outStage;
    }
};

TEST_F(DocumentSourceOutTest, FailsToParseIncorrectType) {
    BSONObj spec = BSON("$out" << 1);
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, 16990);

    spec = BSON("$out" << BSONArray());
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, 16990);
}

TEST_F(DocumentSourceOutTest, AcceptsStringArgument) {
    BSONObj spec = BSON("$out"
                        << "some_collection");
    auto outStage = createOutStage(spec);
    ASSERT_EQ(outStage->getOutputNs().coll(), "some_collection");
}

TEST_F(DocumentSourceOutTest, SerializeDefaultsModeRecreateCollection) {
    BSONObj spec = BSON("$out"
                        << "some_collection");
    auto outStage = createOutStage(spec);
    auto serialized = outStage->serialize().getDocument();
    ASSERT_EQ(serialized["$out"][kModeFieldName].getStringData(), kDefaultMode);

    // Make sure we can reparse the serialized BSON.
    auto reparsedOutStage = createOutStage(serialized.toBson());
    auto reSerialized = reparsedOutStage->serialize().getDocument();
    ASSERT_EQ(reSerialized["$out"][kModeFieldName].getStringData(), kDefaultMode);
}

TEST_F(DocumentSourceOutTest, SerializeUniqueKeyDefaultsToId) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "target"
                                       << "mode"
                                       << kDefaultMode));
    auto outStage = createOutStage(spec);
    auto serialized = outStage->serialize().getDocument();
    ASSERT_EQ(serialized["$out"][kModeFieldName].getStringData(), kDefaultMode);
    ASSERT_DOCUMENT_EQ(serialized["$out"][kUniqueKeyFieldName].getDocument(),
                       (Document{{"_id", 1}}));

    spec = BSON("$out"
                << "some_collection");
    outStage = createOutStage(spec);
    serialized = outStage->serialize().getDocument();
    ASSERT_EQ(serialized["$out"][kModeFieldName].getStringData(), kDefaultMode);
    ASSERT_DOCUMENT_EQ(serialized["$out"][kUniqueKeyFieldName].getDocument(),
                       (Document{{"_id", 1}}));
}

TEST_F(DocumentSourceOutTest, SerializeCompoundUniqueKey) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "target"
                                       << "mode"
                                       << kDefaultMode
                                       << "uniqueKey"
                                       << BSON("_id" << 1 << "shardKey" << 1)));
    auto outStage = createOutStage(spec);
    auto serialized = outStage->serialize().getDocument();
    ASSERT_EQ(serialized["$out"][kModeFieldName].getStringData(), kDefaultMode);
    ASSERT_DOCUMENT_EQ(serialized["$out"][kUniqueKeyFieldName].getDocument(),
                       (Document{{"_id", 1}, {"shardKey", 1}}));
}

TEST_F(DocumentSourceOutTest, SerializeDottedPathUniqueKey) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "target"
                                       << "mode"
                                       << kDefaultMode
                                       << "uniqueKey"
                                       << BSON("_id" << 1 << "a.b" << 1)));
    auto outStage = createOutStage(spec);
    auto serialized = outStage->serialize().getDocument();
    ASSERT_EQ(serialized["$out"][kModeFieldName].getStringData(), kDefaultMode);
    ASSERT_DOCUMENT_EQ(serialized["$out"][kUniqueKeyFieldName].getDocument(),
                       (Document{{"_id", 1}, {"a.b", 1}}));

    spec = BSON("$out" << BSON("to"
                               << "target"
                               << "mode"
                               << kDefaultMode
                               << "uniqueKey"
                               << BSON("_id.a" << 1)));
    outStage = createOutStage(spec);
    serialized = outStage->serialize().getDocument();
    ASSERT_EQ(serialized["$out"][kModeFieldName].getStringData(), kDefaultMode);
    ASSERT_DOCUMENT_EQ(serialized["$out"][kUniqueKeyFieldName].getDocument(),
                       (Document{{"_id.a", 1}}));
}

TEST_F(DocumentSourceOutTest, SerializeDottedPathUniqueKeySharedPrefix) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "target"
                                       << "mode"
                                       << kDefaultMode
                                       << "uniqueKey"
                                       << BSON("_id" << 1 << "a.b" << 1 << "a.c" << 1)));
    auto outStage = createOutStage(spec);
    auto serialized = outStage->serialize().getDocument();
    ASSERT_EQ(serialized["$out"][kModeFieldName].getStringData(), kDefaultMode);
    ASSERT_DOCUMENT_EQ(serialized["$out"][kUniqueKeyFieldName].getDocument(),
                       (Document{{"_id", 1}, {"a.b", 1}, {"a.c", 1}}));
}

TEST_F(DocumentSourceOutTest, SerializeDuplicateUniqueKey) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "target"
                                       << "mode"
                                       << kDefaultMode
                                       << "uniqueKey"
                                       << BSON("_id" << 1 << "dupKey" << 1 << "dupKey" << 1)));
    auto outStage = createOutStage(spec);
    auto serialized = outStage->serialize().getDocument();
    ASSERT_EQ(serialized["$out"][kModeFieldName].getStringData(), kDefaultMode);
    ASSERT_DOCUMENT_EQ(serialized["$out"][kUniqueKeyFieldName].getDocument(),
                       (Document{{"_id", 1}, {"dupKey", 1}}));
}

// TODO SERVER-36367: Nested objects should not be allowed in the uniqueKey spec.
TEST_F(DocumentSourceOutTest, SerializeNestedObjectInUniqueKey) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "target"
                                       << "mode"
                                       << kDefaultMode
                                       << "uniqueKey"
                                       << BSON("_id" << 1 << "shardKey"
                                                     << BSON("subShardKey" << 1))));
    auto outStage = createOutStage(spec);
    auto serialized = outStage->serialize().getDocument();
    ASSERT_EQ(serialized["$out"][kModeFieldName].getStringData(), kDefaultMode);
    ASSERT_DOCUMENT_EQ(serialized["$out"][kUniqueKeyFieldName].getDocument(),
                       (Document{{"_id", 1}, {"shardKey", 1}}));
}

TEST_F(DocumentSourceOutTest, FailsToParseIfToIsNotString) {
    BSONObj spec = BSON("$out" << BSONObj());
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, 40414);

    spec = BSON("$out" << BSON("to" << 1));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$out" << BSON("to" << BSON("a" << 1)));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceOutTest, FailsToParseIfToIsNotAValidUserCollection) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "$test"
                                       << "mode"
                                       << kDefaultMode));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, 17385);

    spec = BSON("$out" << BSON("to"
                               << "system.views"
                               << "mode"
                               << kDefaultMode));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, 17385);

    spec = BSON("$out" << BSON("to"
                               << ".test."
                               << "mode"
                               << kDefaultMode));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceOutTest, FailsToParseIfDbIsNotString) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "test"
                                       << "mode"
                                       << kDefaultMode
                                       << "db"
                                       << true));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$out" << BSON("to"
                               << "test"
                               << "mode"
                               << kDefaultMode
                               << "db"
                               << BSONArray()));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$out" << BSON("to"
                               << "test"
                               << "mode"
                               << kDefaultMode
                               << "db"
                               << BSON(""
                                       << "test")));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceOutTest, FailsToParseIfDbIsNotAValidDatabaseName) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "test"
                                       << "mode"
                                       << kInsertDocumentsMode
                                       << "db"
                                       << "$invalid"));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, 17385);

    spec = BSON("$out" << BSON("to"
                               << "test"
                               << "mode"
                               << kInsertDocumentsMode
                               << "db"
                               << ".test"));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceOutTest, FailsToParseIfModeIsNotString) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "test"
                                       << "mode"
                                       << true));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$out" << BSON("to"
                               << "test"
                               << "mode"
                               << BSONArray()));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$out" << BSON("to"
                               << "test"
                               << "mode"
                               << BSON("" << kDefaultMode)));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceOutTest, FailsToParseIfModeIsUnsupportedString) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "test"
                                       << "mode"
                                       << "unsupported"));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::BadValue);

    spec = BSON("$out" << BSON("to"
                               << "test"
                               << "mode"
                               << "merge"));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::BadValue);
}

TEST_F(DocumentSourceOutTest, FailsToParseIfUniqueKeyIsNotAnObject) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "test"
                                       << "mode"
                                       << kDefaultMode
                                       << "uniqueKey"
                                       << 1));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$out" << BSON("to"
                               << "test"
                               << "mode"
                               << kDefaultMode
                               << "uniqueKey"
                               << BSONArray()));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$out" << BSON("to"
                               << "test"
                               << "mode"
                               << kDefaultMode
                               << "uniqueKey"
                               << "_id"));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceOutTest, CorrectlyUsesTargetDbThatMatchesAggregationDb) {
    const auto targetDbSameAsAggregationDb = getExpCtx()->ns.db();
    const auto targetColl = "test"_sd;
    BSONObj spec = BSON("$out" << BSON("to" << targetColl << "mode" << kDefaultMode << "db"
                                            << targetDbSameAsAggregationDb));

    auto outStage = createOutStage(spec);
    ASSERT_EQ(outStage->getOutputNs().db(), targetDbSameAsAggregationDb);
    ASSERT_EQ(outStage->getOutputNs().coll(), targetColl);
}

// TODO (SERVER-50939): Allow "replaceCollection" to a foreign database.
TEST_F(DocumentSourceOutTest, CorrectlyUsesForeignTargetDb) {
    const auto foreignDb = "someOtherDb"_sd;
    const auto targetColl = "test"_sd;
    BSONObj spec =
        BSON("$out" << BSON("to" << targetColl << "mode" << kDefaultMode << "db" << foreignDb));

    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, 50939);
}
}  // namespace
}  // namespace mongo
