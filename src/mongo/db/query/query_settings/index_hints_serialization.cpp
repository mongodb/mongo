// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/index_hints_serialization.h"

#include "mongo/db/query/query_settings/query_settings_gen.h"

#include <string_view>

namespace mongo::query_settings::index_hints {

void serialize(const IndexHintSpecs& indexHints,
               std::string_view fieldName,
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
