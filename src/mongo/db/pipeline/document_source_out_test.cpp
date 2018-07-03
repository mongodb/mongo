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

class DocumentSourceOutTest : public AggregationContextFixture {
public:
    intrusive_ptr<DocumentSource> createOutStage(BSONObj spec) {
        auto specElem = spec.firstElement();
        return DocumentSourceOut::createFromBson(specElem, getExpCtx());
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
    auto docSource = createOutStage(spec);
    auto outStage = dynamic_cast<DocumentSourceOut*>(docSource.get());
    ASSERT_EQ(outStage->getOutputNs().coll(), "some_collection");
}

TEST_F(DocumentSourceOutTest, SerializeDefaultsModeInsertAndDropTargetTrue) {
    BSONObj spec = BSON("$out"
                        << "some_collection");
    auto docSource = createOutStage(spec);
    auto outStage = dynamic_cast<DocumentSourceOut*>(docSource.get());
    auto serialized = outStage->serialize().getDocument();
    ASSERT_EQ(serialized["$out"][DocumentSourceOutSpec::kDropTargetFieldName].getBool(), true);
    ASSERT_EQ(serialized["$out"][DocumentSourceOutSpec::kModeFieldName].getStringData(),
              "insert"_sd);

    // Make sure we can reparse the serialized BSON.
    auto reparsedDocSource = createOutStage(serialized.toBson());
    auto reparsedOut = dynamic_cast<DocumentSourceOut*>(reparsedDocSource.get());
    auto reSerialized = reparsedOut->serialize().getDocument();
    ASSERT_EQ(reSerialized["$out"][DocumentSourceOutSpec::kDropTargetFieldName].getBool(), true);
    ASSERT_EQ(reSerialized["$out"][DocumentSourceOutSpec::kModeFieldName].getStringData(),
              "insert"_sd);
}

TEST_F(DocumentSourceOutTest, SerializeUniqueKeyOnlyIfSpecified) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "target"
                                       << "mode"
                                       << "insert"
                                       << "dropTarget"
                                       << true
                                       << "uniqueKey"
                                       << BSON("_id" << 1 << "shardKey" << 1)));
    auto docSource = createOutStage(spec);
    auto outStage = dynamic_cast<DocumentSourceOut*>(docSource.get());
    auto serialized = outStage->serialize().getDocument();
    ASSERT_EQ(serialized["$out"][DocumentSourceOutSpec::kDropTargetFieldName].getBool(), true);
    ASSERT_EQ(serialized["$out"][DocumentSourceOutSpec::kModeFieldName].getStringData(),
              "insert"_sd);
    ASSERT_DOCUMENT_EQ(serialized["$out"][DocumentSourceOutSpec::kUniqueKeyFieldName].getDocument(),
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
                                       << "insert"
                                       << "dropTarget"
                                       << true));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, 17385);

    spec = BSON("$out" << BSON("to"
                               << "system.views"
                               << "mode"
                               << "insert"
                               << "dropTarget"
                               << true));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, 17385);

    spec = BSON("$out" << BSON("to"
                               << ".test."
                               << "mode"
                               << "insert"
                               << "dropTarget"
                               << true));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceOutTest, FailsToParseIfDropTargetIsNotBoolean) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "test"
                                       << "mode"
                                       << "insert"
                                       << "dropTarget"
                                       << "invalid"));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$out" << BSON("to"
                               << "test"
                               << "mode"
                               << "insert"
                               << "dropTarget"
                               << BSONArray()));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$out" << BSON("to"
                               << "test"
                               << "mode"
                               << "insert"
                               << "dropTarget"
                               << 1));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceOutTest, FailsToParseIfDbIsNotString) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "test"
                                       << "mode"
                                       << "insert"
                                       << "db"
                                       << true));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$out" << BSON("to"
                               << "test"
                               << "mode"
                               << "insert"
                               << "db"
                               << BSONArray()));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$out" << BSON("to"
                               << "test"
                               << "mode"
                               << "insert"
                               << "db"
                               << BSON(""
                                       << "test")));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceOutTest, FailsToParseIfDbIsNotAValidDatabaseName) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "test"
                                       << "mode"
                                       << "insert"
                                       << "dropTarget"
                                       << true
                                       << "db"
                                       << "$invalid"));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, 17385);

    spec = BSON("$out" << BSON("to"
                               << "test"
                               << "mode"
                               << "insert"
                               << "dropTarget"
                               << true
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
                               << BSON(""
                                       << "insert")));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceOutTest, FailsToParseIfModeIsUnsupportedString) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "test"
                                       << "mode"
                                       << "not_insert"));
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
                                       << "insert"
                                       << "uniqueKey"
                                       << 1));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$out" << BSON("to"
                               << "test"
                               << "mode"
                               << "insert"
                               << "uniqueKey"
                               << BSONArray()));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);

    spec = BSON("$out" << BSON("to"
                               << "test"
                               << "mode"
                               << "insert"
                               << "uniqueKey"
                               << "_id"));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceOutTest, CorrectlyUsesTargetDbIfSpecified) {
    const auto targetDb = "someOtherDb"_sd;
    const auto targetColl = "test"_sd;
    BSONObj spec = BSON("$out" << BSON("to" << targetColl << "mode"
                                            << "insert"
                                            << "dropTarget"
                                            << true
                                            << "db"
                                            << targetDb));

    auto docSource = createOutStage(spec);
    auto outStage = dynamic_cast<DocumentSourceOut*>(docSource.get());
    ASSERT_EQ(outStage->getOutputNs().db(), targetDb);
    ASSERT_EQ(outStage->getOutputNs().coll(), targetColl);
}

TEST_F(DocumentSourceOutTest, DropTargetMustBeTrue) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "test"
                                       << "mode"
                                       << "insert"
                                       << "dropTarget"
                                       << false));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::InvalidOptions);
}

TEST_F(DocumentSourceOutTest, ModeMustBeInsert) {
    BSONObj spec = BSON("$out" << BSON("to"
                                       << "test"
                                       << "mode"
                                       << "replace"
                                       << "dropTarget"
                                       << true));
    ASSERT_THROWS_CODE(createOutStage(spec), AssertionException, ErrorCodes::InvalidOptions);
}

}  // namespace
}  // namespace mongo
