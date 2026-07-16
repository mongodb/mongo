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

#include "mongo/s/commands/document_shard_key_query_conversion.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/platform/compiler.h"

namespace mongo {

BSONObj convertDocumentIntoQuery(const BSONObj& document) {
    BSONObjBuilder query;
    BSONArrayBuilder exprQuery;

    for (BSONElement elem : document) {
        const StringData fieldName = elem.fieldNameStringData();

        const bool shouldWrapIntoGetField = fieldName.starts_with("$");
        if (MONGO_unlikely(shouldWrapIntoGetField)) {
            exprQuery.append(
                BSON("$eq" << BSON_ARRAY(
                         BSON("$getField" << BSON("input"
                                                  << "$$ROOT"
                                                  << "field" << BSON("$literal" << fieldName)))
                         << BSON("$literal" << elem))));
        } else {
            const bool shouldWrapIntoEq = elem.type() == BSONType::Object &&
                elem.Obj().firstElementFieldNameStringData().starts_with("$");
            if (shouldWrapIntoEq) {
                BSONObjBuilder eqOperator = query.subobjStart(fieldName);
                eqOperator.appendAs(elem, "$eq");
                eqOperator.doneFast();
            } else {
                query.append(elem);
            }
        }
    }

    if (auto exprQueryArray = exprQuery.arr(); !exprQueryArray.isEmpty()) {
        query.append("$expr", BSON("$and" << exprQueryArray));
    }

    return query.obj();
}

bool containsDollarPrefixedFieldNamesOnTopLevel(const Document& document) {
    FieldIterator it(document);
    while (it.more()) {
        auto fieldPair = it.next();
        if (fieldPair.first.starts_with("$")) {
            return true;
        }
        const Value& value = fieldPair.second;
        if (value.isObject()) {
            FieldIterator subIt(value.getDocument());
            if (subIt.more() && subIt.next().first.starts_with("$")) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace mongo
