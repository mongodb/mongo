/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/extensions_callback.h"

#include "mongo/bson/util/bson_extract.h"

namespace mongo {

StatusWith<TextMatchExpressionBase::TextParams>
ExtensionsCallback::extractTextMatchExpressionParams(BSONElement text) {
    TextMatchExpressionBase::TextParams params;
    if (text.type() != Object) {
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
        invariantOK(languageStatus);
        expectedFieldCount++;
    }

    Status caseSensitiveStatus =
        bsonExtractBooleanField(queryObj, "$caseSensitive", &params.caseSensitive);
    if (caseSensitiveStatus == ErrorCodes::TypeMismatch) {
        return caseSensitiveStatus;
    } else if (caseSensitiveStatus == ErrorCodes::NoSuchKey) {
        params.caseSensitive = TextMatchExpressionBase::kCaseSensitiveDefault;
    } else {
        invariantOK(caseSensitiveStatus);
        expectedFieldCount++;
    }

    Status diacriticSensitiveStatus =
        bsonExtractBooleanField(queryObj, "$diacriticSensitive", &params.diacriticSensitive);
    if (diacriticSensitiveStatus == ErrorCodes::TypeMismatch) {
        return diacriticSensitiveStatus;
    } else if (diacriticSensitiveStatus == ErrorCodes::NoSuchKey) {
        params.diacriticSensitive = TextMatchExpressionBase::kDiacriticSensitiveDefault;
    } else {
        invariantOK(diacriticSensitiveStatus);
        expectedFieldCount++;
    }

    if (queryObj.nFields() != expectedFieldCount) {
        return {ErrorCodes::BadValue, "extra fields in $text"};
    }

    return {std::move(params)};
}

StatusWith<WhereMatchExpressionBase::WhereParams>
ExtensionsCallback::extractWhereMatchExpressionParams(BSONElement where) {
    WhereMatchExpressionBase::WhereParams params;

    switch (where.type()) {
        case mongo::String:
        case mongo::Code:
            params.code = where._asCode();
            params.scope = BSONObj();
            break;
        case mongo::CodeWScope:
            params.code = where._asCode();
            params.scope = where.codeWScopeObject().getOwned();
            break;
        default:
            return {ErrorCodes::BadValue, "$where got bad type"};
    }

    if (params.code.empty()) {
        return {ErrorCodes::BadValue, "code for $where cannot be empty"};
    }

    return params;
}

}  // namespace mongo
