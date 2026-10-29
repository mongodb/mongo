// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_merge_spec.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/database_name.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_merge_gen.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {

NamespaceString mergeTargetNssParseFromBSON(boost::optional<TenantId> tenantId,
                                            const BSONElement& elem,
                                            const SerializationContext& sc) {
    uassert(51178,
            fmt::format("{} 'into' field  must be either a string or an object, "
                        "but found {}",
                        DocumentSourceMerge::kStageName,
                        typeName(elem.type())),
            elem.type() == BSONType::string || elem.type() == BSONType::object);

    if (elem.type() == BSONType::string) {
        uassert(5786800,
                fmt::format("{} 'into' field cannot be an empty string",
                            DocumentSourceMerge::kStageName),
                !elem.valueStringData().empty());
        return NamespaceStringUtil::deserialize(tenantId, "", elem.valueStringData(), sc);
    }
    const auto vts = tenantId
        ? boost::make_optional(auth::ValidatedTenancyScopeFactory::create(
              *tenantId, auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{}))
        : boost::none;
    auto spec = NamespaceSpec::parse(
        elem.embeddedObject(), IDLParserContext(elem.fieldNameStringData(), vts, tenantId, sc));
    auto coll = spec.getColl();
    uassert(
        5786801,
        fmt::format("{} 'into' field must specify a 'coll' that is not empty, null or undefined",
                    DocumentSourceMerge::kStageName),
        coll && !coll->empty());

    return NamespaceStringUtil::deserialize(
        spec.getDb().value_or(DatabaseNameUtil::deserialize(tenantId, "", sc)), *coll);
}

void mergeTargetNssSerializeToBSON(const NamespaceString& targetNss,
                                   std::string_view fieldName,
                                   BSONObjBuilder* bob,
                                   const SerializationContext& sc,
                                   const query_shape::SerializationOptions& opts) {
    bob->append(
        fieldName,
        BSON("db" << opts.serializeIdentifier(DatabaseNameUtil::serialize(targetNss.dbName(), sc))
                  << "coll" << opts.serializeIdentifier(targetNss.coll())));
}

std::vector<std::string> mergeOnFieldsParseFromBSON(const BSONElement& elem) {
    std::vector<std::string> fields;

    uassert(51186,
            fmt::format("{} 'on' field  must be either a string or an array of strings, "
                        "but found {}",
                        DocumentSourceMerge::kStageName,
                        typeName(elem.type())),
            elem.type() == BSONType::string || elem.type() == BSONType::array);

    if (elem.type() == BSONType::string) {
        fields.push_back(elem.str());
    } else {
        BSONObjIterator iter(elem.Obj());
        while (iter.more()) {
            const BSONElement matchByElem = iter.next();
            uassert(51134,
                    fmt::format("{} 'on' array elements must be strings, but found {}",
                                DocumentSourceMerge::kStageName,
                                typeName(matchByElem.type())),
                    matchByElem.type() == BSONType::string);
            fields.push_back(matchByElem.str());
        }
    }

    uassert(51187,
            fmt::format("If explicitly specifying {} 'on', must include at least one field",
                        DocumentSourceMerge::kStageName),
            fields.size() > 0);

    return fields;
}

void mergeOnFieldsSerializeToBSON(const std::vector<std::string>& fields,
                                  std::string_view fieldName,
                                  BSONObjBuilder* bob,
                                  const query_shape::SerializationOptions& opts) {
    if (fields.size() == 1) {
        bob->append(fieldName, opts.serializeFieldPathFromString(fields.front()));
    } else {
        bob->append(fieldName, opts.serializeFieldPathFromString(fields));
    }
}

MergeWhenMatchedPolicy mergeWhenMatchedParseFromBSON(const BSONElement& elem) {
    uassert(51191,
            fmt::format("{} 'whenMatched' field  must be either a string or an array, "
                        "but found {}",
                        DocumentSourceMerge::kStageName,
                        typeName(elem.type())),
            elem.type() == BSONType::string || elem.type() == BSONType::array);

    if (elem.type() == BSONType::array) {
        return {MergeWhenMatchedModeEnum::kPipeline, parsePipelineFromBSON(elem)};
    }

    IDLParserContext ctx{DocumentSourceMergeSpec::kWhenMatchedFieldName};
    auto value = elem.valueStringData();
    auto mode = idl::deserialize<MergeWhenMatchedModeEnum>(value, ctx);

    // The 'kPipeline' mode cannot be specified explicitly, a custom pipeline definition must be
    // used instead.
    if (mode == MergeWhenMatchedModeEnum::kPipeline) {
        ctx.throwBadEnumValue(value);
    }
    return {mode};
}

void mergeWhenMatchedSerializeToBSON(const MergeWhenMatchedPolicy& policy,
                                     std::string_view fieldName,
                                     BSONObjBuilder* bob) {
    if (policy.mode == MergeWhenMatchedModeEnum::kPipeline) {
        tassert(11282973, "Merge policy lacks the pipeline", policy.pipeline);
        bob->append(fieldName, *policy.pipeline);
    } else {
        bob->append(fieldName, idl::serialize(policy.mode));
    }
}
}  // namespace mongo
