// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/extensions_callback.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"

#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

StatusWith<TextMatchExpressionBase::TextParams>
ExtensionsCallback::extractTextMatchExpressionParams(BSONElement text) {
    TextMatchExpressionBase::TextParams params;
    if (text.type() != BSONType::object) {
        return {ErrorCodes::BadValue, "$text expects an object"};
    }
    BSONObj queryObj = text.Obj();

    //
    // Parse required fields.
    //

    Status queryStatus = bsonExtractStringField(queryObj, "$search", &params.query);
    if (!queryStatus.isOK()) {
        return queryStatus;
    }

    //
    // Parse optional fields.
    //

    int expectedFieldCount = 1;

    Status languageStatus = bsonExtractStringField(queryObj, "$language", &params.language);
    if (languageStatus == ErrorCodes::TypeMismatch) {
        return languageStatus;
    } else if (languageStatus == ErrorCodes::NoSuchKey) {
        params.language = std::string();
    } else {
        tassert(languageStatus);
        expectedFieldCount++;
    }

    Status caseSensitiveStatus =
        bsonExtractBooleanField(queryObj, "$caseSensitive", &params.caseSensitive);
    if (caseSensitiveStatus == ErrorCodes::TypeMismatch) {
        return caseSensitiveStatus;
    } else if (caseSensitiveStatus == ErrorCodes::NoSuchKey) {
        params.caseSensitive = TextMatchExpressionBase::kCaseSensitiveDefault;
    } else {
        tassert(caseSensitiveStatus);
        expectedFieldCount++;
    }

    Status diacriticSensitiveStatus =
        bsonExtractBooleanField(queryObj, "$diacriticSensitive", &params.diacriticSensitive);
    if (diacriticSensitiveStatus == ErrorCodes::TypeMismatch) {
        return diacriticSensitiveStatus;
    } else if (diacriticSensitiveStatus == ErrorCodes::NoSuchKey) {
        params.diacriticSensitive = TextMatchExpressionBase::kDiacriticSensitiveDefault;
    } else {
        tassert(diacriticSensitiveStatus);
        expectedFieldCount++;
    }

    if (queryObj.nFields() != expectedFieldCount) {
        return {ErrorCodes::BadValue, "extra fields in $text"};
    }

    return {std::move(params)};
}

StatusWithMatchExpression ExtensionsCallback::parseText(BSONElement text) const {
    auto textParams = extractTextMatchExpressionParams(text);
    if (!textParams.isOK()) {
        return textParams.getStatus();
    }
    return createText(std::move(textParams.getValue()));
}

StatusWith<WhereMatchExpressionBase::WhereParams>
ExtensionsCallback::extractWhereMatchExpressionParams(BSONElement where) {
    WhereMatchExpressionBase::WhereParams params;

    switch (where.type()) {
        case BSONType::string:
        case BSONType::code:
            params.code = where._asCode();
            break;
        case BSONType::codeWScope:
            uasserted(4649201, "$where no longer supports deprecated BSON type CodeWScope");
            break;
        default:
            return {ErrorCodes::BadValue, "$where got bad type"};
    }

    if (params.code.empty()) {
        return {ErrorCodes::BadValue, "code for $where cannot be empty"};
    }

    return params;
}

StatusWithMatchExpression ExtensionsCallback::parseWhere(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, BSONElement where) const {
    auto whereParams = extractWhereMatchExpressionParams(where);
    if (!whereParams.isOK()) {
        return whereParams.getStatus();
    }
    return {createWhere(expCtx, std::move(whereParams.getValue()))};
}

}  // namespace mongo
