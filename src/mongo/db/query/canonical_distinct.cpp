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

#include <boost/cstdint.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/curop.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/canonical_distinct.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/distinct_command_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/max_time_ms_parser.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

namespace mongo {

const char CanonicalDistinct::kKeyField[] = "key";
const char CanonicalDistinct::kQueryField[] = "query";
const char CanonicalDistinct::kCollationField[] = "collation";
const char CanonicalDistinct::kUnwoundArrayFieldForViewUnwind[] = "_internalUnwoundArray";
const char CanonicalDistinct::kHintField[] = "hint";

namespace {
/**
 * Add the stages that pull all values along a path and puts them into an array. Includes the
 * necessary unwind stage that can turn those into individual documents.
 */
void addReplaceRootForDistinct(BSONArrayBuilder* pipelineBuilder, const FieldPath& path) {
    BSONObjBuilder reshapeStageBuilder(pipelineBuilder->subobjStart());
    reshapeStageBuilder.append(
        DocumentSourceReplaceRoot::kStageName,
        BSON("newRoot" << BSON(CanonicalDistinct::kUnwoundArrayFieldForViewUnwind
                               << BSON("$_internalFindAllValuesAtPath" << path.fullPath()))));
    reshapeStageBuilder.doneFast();
    BSONObjBuilder unwindStageBuilder(pipelineBuilder->subobjStart());
    {
        BSONObjBuilder unwindBuilder(unwindStageBuilder.subobjStart("$unwind"));
        unwindBuilder.append(
            "path", str::stream() << "$" << CanonicalDistinct::kUnwoundArrayFieldForViewUnwind);
        unwindBuilder.append("preserveNullAndEmptyArrays", true);
    }
}

/**
 * Helper for when converting a distinct() to an aggregation pipeline. This function may add a
 * $match stage enforcing that intermediate subpaths are objects so that no implicit array
 * traversal happens later on. The $match stage is only added when the path is dotted (e.g. "a.b"
 * but for "xyz").
 *
 * See comments in CanonicalDistinct::asAggregationCommand() for more detailed explanation.
 */
void addMatchRemovingNestedArrays(BSONArrayBuilder* pipelineBuilder, const FieldPath& unwindPath) {
    if (unwindPath.getPathLength() == 1) {
        return;
    }
    invariant(unwindPath.getPathLength() > 1);

    BSONObjBuilder matchBuilder(pipelineBuilder->subobjStart());
    BSONObjBuilder predicateBuilder(matchBuilder.subobjStart("$match"));


    for (size_t i = 0; i < unwindPath.getPathLength() - 1; ++i) {
        StringData pathPrefix = unwindPath.getSubpath(i);
        // Add a clause to the $match predicate requiring that intermediate paths are objects so
        // that no implicit array traversal happens.
        predicateBuilder.append(pathPrefix,
                                BSON("$_internalSchemaType"
                                     << "object"));
    }

    predicateBuilder.doneFast();
    matchBuilder.doneFast();
}

/**
 * Checks dotted field for a projection and truncates the field name if we could be projecting on an
 * array element. Sets 'isIDOut' to true if the projection is on a sub document of _id. For example,
 * _id.a.2, _id.b.c.
 */
std::string getProjectedDottedField(const std::string& field, bool* isIDOut) {
    // Check if field contains an array index.
    std::vector<std::string> res;
    str::splitStringDelim(field, &res, '.');

    // Since we could exit early from the loop,
    // we should check _id here and set '*isIDOut' accordingly.
    *isIDOut = ("_id" == res[0]);

    // Skip the first dotted component. If the field starts
    // with a number, the number cannot be an array index.
    int arrayIndex = 0;
    for (size_t i = 1; i < res.size(); ++i) {
        if (mongo::NumberParser().base(10)(res[i], &arrayIndex).isOK()) {
            // Array indices cannot be negative numbers (this is not $slice).
            // Negative numbers are allowed as field names.
            if (arrayIndex >= 0) {
                // Generate prefix of field up to (but not including) array index.
                std::vector<std::string> prefixStrings(res);
                prefixStrings.resize(i);
                // Reset projectedField. Instead of overwriting, joinStringDelim() appends joined
                // string
                // to the end of projectedField.
                std::string projectedField;
                str::joinStringDelim(prefixStrings, &projectedField, '.');
                return projectedField;
            }
        }
    }

    return field;
}

/**
 * Creates a projection spec for a distinct command from the requested field. In most cases, the
 * projection spec will be {_id: 0, key: 1}.
 * The exceptions are:
 * 1) When the requested field is '_id', the projection spec will {_id: 1}.
 * 2) When the requested field could be an array element (eg. a.0), the projected field will be the
 *    prefix of the field up to the array element. For example, a.b.2 => {_id: 0, 'a.b': 1} Note
 *    that we can't use a $slice projection because the distinct command filters the results from
 *    the executor using the dotted field name. Using $slice will re-order the documents in the
 *    array in the results.
 */
BSONObj getDistinctProjection(const std::string& field) {
    std::string projectedField(field);

    bool isID = false;
    if ("_id" == field) {
        isID = true;
    } else if (str::contains(field, '.')) {
        projectedField = getProjectedDottedField(field, &isID);
    }
    BSONObjBuilder bob;
    if (!isID) {
        bob.append("_id", 0);
    }
    bob.append(projectedField, 1);
    return bob.obj();
}
}  // namespace

StatusWith<BSONObj> CanonicalDistinct::asAggregationCommand() const {
    BSONObjBuilder aggregationBuilder;

    invariant(_query);
    const FindCommandRequest& findCommand = _query->getFindCommandRequest();
    tassert(ErrorCodes::BadValue,
            "Unsupported type UUID for namespace",
            findCommand.getNamespaceOrUUID().isNamespaceString());
    aggregationBuilder.append("aggregate", findCommand.getNamespaceOrUUID().nss().coll());

    // Build a pipeline that accomplishes the distinct request. The building code constructs a
    // pipeline that looks like this, assuming the distinct is on the key "a.b.c"
    //
    //      [
    //          { $match: { ... } },
    //          { $replaceRoot: {newRoot: {$_internalFindAllValuesAtPath: "a.b.c"}}},
    //          { $unwind: { path: "_internalUnwoundField", preserveNullAndEmptyArrays: true } },
    //          { $group: { _id: null, distinct: { $addToSet: "$<key>" } } }
    //      ]
    //
    // The purpose of the intermediate $replaceRoot and $unwind stages is to deal with cases
    // where there is an array along the distinct path. For example, if we're distincting on "a.b"
    // and have a document like {a: [{b: 1}, {b: 2}]}, distinct() should produce two values: 1
    // and 2.
    BSONArrayBuilder pipelineBuilder(aggregationBuilder.subarrayStart("pipeline"));
    if (!findCommand.getFilter().isEmpty()) {
        BSONObjBuilder matchStageBuilder(pipelineBuilder.subobjStart());
        matchStageBuilder.append("$match", findCommand.getFilter());
        matchStageBuilder.doneFast();
    }

    FieldPath path(_key);
    addReplaceRootForDistinct(&pipelineBuilder, path);

    BSONObjBuilder groupStageBuilder(pipelineBuilder.subobjStart());
    {
        BSONObjBuilder groupBuilder(groupStageBuilder.subobjStart("$group"));
        groupBuilder.appendNull("_id");
        {
            BSONObjBuilder distinctBuilder(groupBuilder.subobjStart("distinct"));
            distinctBuilder.append("$addToSet",
                                   str::stream() << "$" << kUnwoundArrayFieldForViewUnwind);
            distinctBuilder.doneFast();
        }
        groupBuilder.doneFast();
    }
    groupStageBuilder.doneFast();
    pipelineBuilder.doneFast();

    aggregationBuilder.append(kCollationField, findCommand.getCollation());
    aggregationBuilder.append(kHintField, findCommand.getHint());

    int maxTimeMS = findCommand.getMaxTimeMS() ? static_cast<int>(*findCommand.getMaxTimeMS()) : 0;
    if (maxTimeMS > 0) {
        aggregationBuilder.append(query_request_helper::cmdOptionMaxTimeMS, maxTimeMS);
    }

    if (findCommand.getReadConcern() && !findCommand.getReadConcern()->isEmpty()) {
        aggregationBuilder.append(repl::ReadConcernArgs::kReadConcernFieldName,
                                  *findCommand.getReadConcern());
    }

    if (!findCommand.getUnwrappedReadPref().isEmpty()) {
        aggregationBuilder.append(query_request_helper::kUnwrappedReadPrefField,
                                  findCommand.getUnwrappedReadPref());
    }

    // Specify the 'cursor' option so that aggregation uses the cursor interface.
    aggregationBuilder.append("cursor", BSONObj());

    return aggregationBuilder.obj();
}

CanonicalDistinct CanonicalDistinct::parse(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           std::unique_ptr<ParsedDistinctCommand> parsedDistinct,
                                           const CollatorInterface* defaultCollator) {

    auto& distinctRequest = *parsedDistinct->distinctCommandRequest;
    uassert(31032,
            "Key field cannot contain an embedded null byte",
            distinctRequest.getKey().find('\0') == std::string::npos);

    auto findRequest = std::make_unique<FindCommandRequest>(expCtx->ns);
    if (auto query = distinctRequest.getQuery()) {
        findRequest->setFilter(query->getOwned());
    }
    findRequest->setProjection(getDistinctProjection(std::string(distinctRequest.getKey())));
    findRequest->setHint(distinctRequest.getHint().getOwned());
    if (auto rc = parsedDistinct->readConcern) {
        findRequest->setReadConcern(rc->getOwned());
    }
    if (parsedDistinct->maxTimeMS) {
        findRequest->setMaxTimeMS(*parsedDistinct->maxTimeMS);
    }
    if (auto collation = distinctRequest.getCollation()) {
        findRequest->setCollation(collation.get().getOwned());
    }
    if (parsedDistinct->queryOptions) {
        findRequest->setUnwrappedReadPref(*parsedDistinct->queryOptions);
    }

    auto parsedFind = uassertStatusOK(
        ParsedFindCommand::withExistingFilter(expCtx,
                                              std::move(parsedDistinct->collator),
                                              std::move(parsedDistinct->query),
                                              std::move(findRequest),
                                              ProjectionPolicies::findProjectionPolicies()));

    auto cq = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = expCtx, .parsedFind = std::move(parsedFind)});

    if (auto collator = expCtx->getCollator()) {
        cq->setCollator(collator->clone());
    }

    return CanonicalDistinct(std::move(cq),
                             distinctRequest.getKey().toString(),
                             distinctRequest.getMirrored().value_or(false),
                             distinctRequest.getSampleId());
}

boost::intrusive_ptr<ExpressionContext> CanonicalDistinct::makeExpressionContext(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const DistinctCommandRequest& distinctCommand,
    const CollatorInterface* defaultCollator,
    boost::optional<ExplainOptions::Verbosity> verbosity) {

    std::unique_ptr<CollatorInterface> collator;
    if (auto collationObj = distinctCommand.getCollation().get_value_or(BSONObj());
        !collationObj.isEmpty()) {
        collator = uassertStatusOK(
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collationObj));
    } else if (defaultCollator) {
        // The 'collPtr' will be null for views, but we don't need to worry about views here. The
        // views will get rewritten into aggregate command and will regenerate the
        // ExpressionContext.
        collator = defaultCollator->clone();
    }
    auto expCtx =
        make_intrusive<ExpressionContext>(opCtx,
                                          distinctCommand,
                                          nss,
                                          std::move(collator),
                                          CurOp::get(opCtx)->dbProfileLevel() > 0,  // mayDbProfile
                                          verbosity);
    return expCtx;
}

}  // namespace mongo
