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
        invariant(languageStatus);
        expectedFieldCount++;
    }

    Status caseSensitiveStatus =
        bsonExtractBooleanField(queryObj, "$caseSensitive", &params.caseSensitive);
    if (caseSensitiveStatus == ErrorCodes::TypeMismatch) {
        return caseSensitiveStatus;
    } else if (caseSensitiveStatus == ErrorCodes::NoSuchKey) {
        params.caseSensitive = TextMatchExpressionBase::kCaseSensitiveDefault;
    } else {
        invariant(caseSensitiveStatus);
        expectedFieldCount++;
    }

    Status diacriticSensitiveStatus =
        bsonExtractBooleanField(queryObj, "$diacriticSensitive", &params.diacriticSensitive);
    if (diacriticSensitiveStatus == ErrorCodes::TypeMismatch) {
        return diacriticSensitiveStatus;
    } else if (diacriticSensitiveStatus == ErrorCodes::NoSuchKey) {
        params.diacriticSensitive = TextMatchExpressionBase::kDiacriticSensitiveDefault;
    } else {
        invariant(diacriticSensitiveStatus);
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
