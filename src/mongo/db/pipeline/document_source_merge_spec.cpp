/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_merge_spec.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/database_name.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_merge_gen.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/namespace_string_util.h"

#include <boost/move/utility_core.hpp>
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
                                   StringData fieldName,
                                   BSONObjBuilder* bob,
                                   const SerializationContext& sc,
                                   const SerializationOptions& opts) {
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
        invariant(elem.type() == BSONType::array);

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
                                  StringData fieldName,
                                  BSONObjBuilder* bob,
                                  const SerializationOptions& opts) {
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

    invariant(elem.type() == BSONType::string);

    IDLParserContext ctx{DocumentSourceMergeSpec::kWhenMatchedFieldName};
    auto value = elem.valueStringData();
    auto mode = MergeWhenMatchedMode_parse(value, ctx);

    // The 'kPipeline' mode cannot be specified explicitly, a custom pipeline definition must be
    // used instead.
    if (mode == MergeWhenMatchedModeEnum::kPipeline) {
        ctx.throwBadEnumValue(value);
    }
    return {mode};
}

void mergeWhenMatchedSerializeToBSON(const MergeWhenMatchedPolicy& policy,
                                     StringData fieldName,
                                     BSONObjBuilder* bob) {
    if (policy.mode == MergeWhenMatchedModeEnum::kPipeline) {
        invariant(policy.pipeline);
        bob->append(fieldName, *policy.pipeline);
    } else {
        bob->append(fieldName, MergeWhenMatchedMode_serializer(policy.mode));
    }
}
}  // namespace mongo
