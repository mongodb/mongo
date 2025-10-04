/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/query_settings/index_hints_serialization.h"

#include "mongo/db/query/query_settings/query_settings_gen.h"

namespace mongo::query_settings::index_hints {

void serialize(const IndexHintSpecs& indexHints,
               StringData fieldName,
               BSONObjBuilder* builder,
               const SerializationContext& context) {
    BSONArrayBuilder arrayBuilder(builder->subarrayStart(fieldName));
    for (const auto& indexHint : indexHints) {
        BSONObjBuilder subObj(arrayBuilder.subobjStart());
        indexHint.serialize(&subObj);
    }
}

IndexHintSpecs parse(boost::optional<TenantId> tenantId,
                     const BSONElement& element,
                     const SerializationContext& context) {
    switch (element.type()) {
        case BSONType::object: {
            IDLParserContext parserContext(
                "IndexHintSpec", boost::none /*vts=*/, tenantId, context);
            return {IndexHintSpec::parse(element.Obj(), parserContext)};
        }
        case BSONType::array: {
            const auto& elements = element.Array();
            IndexHintSpecs result;
            result.reserve(elements.size());
            for (const auto& element : elements) {
                IDLParserContext parserContext(
                    "IndexHintSpec", /*vts=*/boost::none, tenantId, context);
                result.emplace_back(IndexHintSpec::parse(element.Obj(), parserContext));
            }
            return result;
        }
        default:
            uasserted(ErrorCodes::FailedToParse, "'indexHints' must be an object or an array");
    }
}
}  // namespace mongo::query_settings::index_hints
