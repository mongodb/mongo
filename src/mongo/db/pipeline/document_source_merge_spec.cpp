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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_merge_spec.h"

#include <fmt/format.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_merge_gen.h"

namespace mongo {
using namespace fmt::literals;

// TODO SERVER-66708 Ensure the correct tenantId is passed when deserializing the merge target nss.
NamespaceString mergeTargetNssParseFromBSON(const BSONElement& elem) {
    uassert(51178,
            "{} 'into' field  must be either a string or an object, "
            "but found {}"_format(DocumentSourceMerge::kStageName, typeName(elem.type())),
            elem.type() == BSONType::String || elem.type() == BSONType::Object);

    if (elem.type() == BSONType::String) {
        uassert(5786800,
                "{} 'into' field cannot be an empty string"_format(DocumentSourceMerge::kStageName),
                !elem.valueStringData().empty());
        return {"", elem.valueStringData()};
    }

    auto spec = NamespaceSpec::parse({elem.fieldNameStringData()}, elem.embeddedObject());
    auto coll = spec.getColl();
    uassert(5786801,
            "{} 'into' field must specify a 'coll' that is not empty, null or undefined"_format(
                DocumentSourceMerge::kStageName),
            coll && !coll->empty());

    return {spec.getDb().value_or(""), *coll};
}

void mergeTargetNssSerializeToBSON(const NamespaceString& targetNss,
                                   StringData fieldName,
                                   BSONObjBuilder* bob) {
    bob->append(fieldName, BSON("db" << targetNss.dbName().db() << "coll" << targetNss.coll()));
}

std::vector<std::string> mergeOnFieldsParseFromBSON(const BSONElement& elem) {
    std::vector<std::string> fields;

    uassert(51186,
            "{} 'on' field  must be either a string or an array of strings, "
            "but found {}"_format(DocumentSourceMerge::kStageName, typeName(elem.type())),
            elem.type() == BSONType::String || elem.type() == BSONType::Array);

    if (elem.type() == BSONType::String) {
        fields.push_back(elem.str());
    } else {
        invariant(elem.type() == BSONType::Array);

        BSONObjIterator iter(elem.Obj());
        while (iter.more()) {
            const BSONElement matchByElem = iter.next();
            uassert(51134,
                    "{} 'on' array elements must be strings, but found {}"_format(
                        DocumentSourceMerge::kStageName, typeName(matchByElem.type())),
                    matchByElem.type() == BSONType::String);
            fields.push_back(matchByElem.str());
        }
    }

    uassert(51187,
            "If explicitly specifying {} 'on', must include at least one field"_format(
                DocumentSourceMerge::kStageName),
            fields.size() > 0);

    return fields;
}

void mergeOnFieldsSerializeToBSON(const std::vector<std::string>& fields,
                                  StringData fieldName,
                                  BSONObjBuilder* bob) {
    if (fields.size() == 1) {
        bob->append(fieldName, fields.front());
    } else {
        bob->append(fieldName, fields);
    }
}

MergeWhenMatchedPolicy mergeWhenMatchedParseFromBSON(const BSONElement& elem) {
    uassert(51191,
            "{} 'whenMatched' field  must be either a string or an array, "
            "but found {}"_format(DocumentSourceMerge::kStageName, typeName(elem.type())),
            elem.type() == BSONType::String || elem.type() == BSONType::Array);

    if (elem.type() == BSONType::Array) {
        return {MergeWhenMatchedModeEnum::kPipeline, parsePipelineFromBSON(elem)};
    }

    invariant(elem.type() == BSONType::String);

    IDLParserContext ctx{DocumentSourceMergeSpec::kWhenMatchedFieldName};
    auto value = elem.valueStringData();
    auto mode = MergeWhenMatchedMode_parse(ctx, value);

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
