// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/commands/document_shard_key_query_conversion.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/platform/compiler.h"

#include <string_view>

namespace mongo {

BSONObj convertDocumentIntoQuery(const BSONObj& document) {
    BSONObjBuilder query;
    BSONArrayBuilder exprQuery;

    for (BSONElement elem : document) {
        const std::string_view fieldName = elem.fieldNameStringData();

        const bool shouldWrapIntoGetField = fieldName.starts_with("$");
        if (MONGO_unlikely(shouldWrapIntoGetField)) {
            exprQuery.append(
                BSON("$eq" << BSON_ARRAY(
                         BSON("$getField" << BSON("input" << "$$ROOT" << "field"
                                                          << BSON("$literal" << fieldName)))
                         << BSON("$literal" << elem))));
        } else {
            const bool shouldWrapIntoEq = elem.type() == BSONType::object &&
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
