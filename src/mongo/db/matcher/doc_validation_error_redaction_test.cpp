/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/doc_validation_error.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/logv2/log_util.h"
#include "mongo/unittest/unittest.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::doc_validation_error {
namespace {

// RAII guard that restores both redaction flags on destruction.
struct RedactionGuard {
    RedactionGuard(bool redactLogs, bool redactBinDataEncrypt) {
        logv2::setShouldRedactLogs(redactLogs);
        logv2::setShouldRedactBinDataEncrypt(redactBinDataEncrypt);
    }
    ~RedactionGuard() {
        logv2::setShouldRedactLogs(false);
        logv2::setShouldRedactBinDataEncrypt(false);
    }
};

BSONObj makeError(const BSONObj& query, const BSONObj& doc) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->isParsingCollectionValidator = true;
    auto result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    MatchExpression* expr = result.getValue().get();
    ASSERT_FALSE(expr->matchesBSON(doc));
    return generateError(*expr,
                         doc.hasField("_id") ? doc : doc.addField(BSON("_id" << 1).firstElement()),
                         kDefaultMaxDocValidationErrorSize,
                         10);
}

// When neither redaction flag is set, getRedactedDetails() must return the same object as
// getDetails().
TEST(DocValidationRedaction, RedactionDisabled) {
    RedactionGuard guard(false, false);
    BSONObj details = BSON("failingDocumentId" << 1 << "details"
                                               << BSON("operatorName"
                                                       << "$eq"
                                                       << "consideredValue" << 42 << "reason"
                                                       << "comparison failed"));
    DocumentValidationFailureInfo info(details);
    ASSERT_BSONOBJ_EQ(info.getDetails(), info.getRedactedDetails());
}

// With full log redaction enabled, only 'consideredValue' and 'consideredValues' fields should be
// replaced with "###". Other fields (operatorName, specifiedAs, reason) must be left unchanged.
TEST(DocValidationRedaction, OnlyConsideredValueFieldsAreRedacted) {
    RedactionGuard guard(true, false);
    BSONObj details =
        BSON("failingDocumentId" << 1 << "details"
                                 << BSON("operatorName"
                                         << "$eq"
                                         << "specifiedAs" << BSON("a" << 1) << "reason"
                                         << "comparison failed"
                                         << "consideredValue" << 99));
    DocumentValidationFailureInfo info(details);
    BSONObj redacted = info.getRedactedDetails();

    auto d = redacted["details"].Obj();
    ASSERT_EQ(d["operatorName"].str(), "$eq");
    ASSERT_EQ(d["reason"].str(), "comparison failed");
    ASSERT_BSONOBJ_EQ(d["specifiedAs"].Obj(), BSON("a" << 1));
    ASSERT_EQ(d["consideredValue"].str(), "###");
}

// Trivial flat case: a top-level consideredValue field is redacted.
TEST(DocValidationRedaction, TrivialFlatRedaction) {
    RedactionGuard guard(true, false);
    BSONObj details = BSON("consideredValue"
                           << "sensitiveString");
    DocumentValidationFailureInfo info(details);
    BSONObj redacted = info.getRedactedDetails();
    ASSERT_EQ(redacted["consideredValue"].str(), "###");
}

// A $jsonSchema with oneOf produces schemasNotSatisfied entries whose details each contain a
// deeply nested consideredValue. With redaction enabled all consideredValues must be redacted while
// the surrounding structural fields are preserved.
TEST(DocValidationRedaction, NestedConsideredValueRedacted) {
    RedactionGuard guard(true, false);
    BSONObj query = fromjson(
        "{'$jsonSchema': {'properties': {'a': {'oneOf': [{'minimum': 4}, {'maximum': 1}]}}}}");
    BSONObj doc = fromjson("{a: 2}");

    DocumentValidationFailureInfo info(makeError(query, doc));
    BSONObj redacted = info.getRedactedDetails();

    // Navigate: details.schemaRulesNotSatisfied[0]
    //                   .propertiesNotSatisfied[0].details[0]
    //                   .schemasNotSatisfied[i].details[0].consideredValue
    auto schemaRules = redacted["details"].Obj()["schemaRulesNotSatisfied"].Array();
    auto propDetails =
        schemaRules[0].Obj()["propertiesNotSatisfied"].Array()[0].Obj()["details"].Array();
    auto schemasNotSatisfied = propDetails[0].Obj()["schemasNotSatisfied"].Array();
    ASSERT_EQ(schemasNotSatisfied.size(), 2u);
    for (const auto& schema : schemasNotSatisfied) {
        auto inner = schema.Obj()["details"].Array()[0].Obj();
        ASSERT_EQ(inner["consideredValue"].str(), "###");
        ASSERT_EQ(inner["reason"].str(), "comparison failed");
    }
}

// consideredValues (plural) is also redacted. Because redact() recurses into arrays, each element
// of the array is individually replaced with "###".
TEST(DocValidationRedaction, ConsideredValuesArrayRedacted) {
    RedactionGuard guard(true, false);
    BSONObj details =
        BSON("failingDocumentId" << 1 << "details"
                                 << BSON("operatorName"
                                         << "$in"
                                         << "consideredValues" << BSON_ARRAY(1 << 2 << 3)));
    DocumentValidationFailureInfo info(details);
    BSONObj redacted = info.getRedactedDetails();

    auto d = redacted["details"].Obj();
    ASSERT_EQ(d["operatorName"].str(), "$in");
    std::vector<BSONElement> vals = d["consideredValues"].Array();
    ASSERT_EQ(vals.size(), 3u);
    for (const auto& v : vals) {
        ASSERT_EQ(v.str(), "###");
    }
}

// When only shouldRedactBinDataEncrypt is set (not shouldRedactLogs), BinData::Encrypt values
// inside consideredValue are redacted, but plain scalar values are not.
TEST(DocValidationRedaction, BinDataEncryptRedactedWhenOnlyEncryptFlagSet) {
    RedactionGuard guard(false, true);

    const char rawBytes[] = {0x01, 0x02, 0x03, 0x04};
    BSONBinData encryptedValue(rawBytes, sizeof(rawBytes), BinDataType::Encrypt);

    BSONObj details =
        BSON("failingDocumentId" << 1 << "details"
                                 << BSON_ARRAY(BSON("operatorName"
                                                    << "$eq"
                                                    << "reason"
                                                    << "comparison failed"
                                                    << "consideredValue" << encryptedValue)
                                               << BSON("operatorName"
                                                       << "$gt"
                                                       << "reason"
                                                       << "comparison failed"
                                                       << "consideredValue" << 42)));
    DocumentValidationFailureInfo info(details);
    BSONObj redacted = info.getRedactedDetails();

    auto arr = redacted["details"].Array();
    ASSERT_EQ(arr.size(), 2u);
    // The BinData::Encrypt consideredValue must be redacted.
    ASSERT_EQ(arr[0].Obj()["consideredValue"].str(), "###");
    // The plain integer consideredValue must be preserved.
    ASSERT_EQ(arr[1].Obj()["consideredValue"].Int(), 42);
}

}  // namespace
}  // namespace mongo::doc_validation_error
