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

#include "mongo/platform/basic.h"

#include "mongo/db/query/parsed_distinct.h"

#include <memory>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/distinct_command_gen.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/str.h"

namespace mongo {

const char ParsedDistinct::kKeyField[] = "key";
const char ParsedDistinct::kQueryField[] = "query";
const char ParsedDistinct::kCollationField[] = "collation";
const char ParsedDistinct::kUnwoundArrayFieldForViewUnwind[] = "_internalUnwoundArray";

namespace {

/**
 * Add the stages that pull all values along a path and puts them into an array. Includes the
 * necessary unwind stage that can turn those into individual documents.
 */
void addReplaceRootForDistinct(BSONArrayBuilder* pipelineBuilder, const FieldPath& path) {
    BSONObjBuilder reshapeStageBuilder(pipelineBuilder->subobjStart());
    reshapeStageBuilder.append(
        DocumentSourceReplaceRoot::kStageName,
        BSON("newRoot" << BSON(ParsedDistinct::kUnwoundArrayFieldForViewUnwind
                               << BSON("$_internalFindAllValuesAtPath" << path.fullPath()))));
    reshapeStageBuilder.doneFast();
    BSONObjBuilder unwindStageBuilder(pipelineBuilder->subobjStart());
    {
        BSONObjBuilder unwindBuilder(unwindStageBuilder.subobjStart("$unwind"));
        unwindBuilder.append(
            "path", str::stream() << "$" << ParsedDistinct::kUnwoundArrayFieldForViewUnwind);
        unwindBuilder.append("preserveNullAndEmptyArrays", true);
    }
}

/**
 * Helper for when converting a distinct() to an aggregation pipeline. This function may add a
 * $match stage enforcing that intermediate subpaths are objects so that no implicit array
 * traversal happens later on. The $match stage is only added when the path is dotted (e.g. "a.b"
 * but for "xyz").
 *
 * See comments in ParsedDistinct::asAggregationCommand() for more detailed explanation.
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

StatusWith<BSONObj> ParsedDistinct::asAggregationCommand() const {
    BSONObjBuilder aggregationBuilder;

    invariant(_query);
    const FindCommandRequest& findCommand = _query->getFindCommandRequest();
    aggregationBuilder.append(
        "aggregate", findCommand.getNamespaceOrUUID().nss().value_or(NamespaceString()).coll());

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

StatusWith<ParsedDistinct> ParsedDistinct::parse(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 const BSONObj& cmdObj,
                                                 const ExtensionsCallback& extensionsCallback,
                                                 bool isExplain,
                                                 const CollatorInterface* defaultCollator) {
    IDLParserContext ctx("distinct");

    DistinctCommandRequest parsedDistinct(nss);
    try {
        parsedDistinct = DistinctCommandRequest::parse(ctx, cmdObj);
    } catch (...) {
        return exceptionToStatus();
    }

    auto findCommand = std::make_unique<FindCommandRequest>(nss);

    if (parsedDistinct.getKey().find('\0') != std::string::npos) {
        return Status(ErrorCodes::Error(31032), "Key field cannot contain an embedded null byte");
    }

    // Create a projection on the fields needed by the distinct command, so that the query planner
    // will produce a covered plan if possible.
    findCommand->setProjection(getDistinctProjection(std::string(parsedDistinct.getKey())));

    if (auto query = parsedDistinct.getQuery()) {
        findCommand->setFilter(query.get().getOwned());
    }

    if (auto collation = parsedDistinct.getCollation()) {
        findCommand->setCollation(collation.get().getOwned());
    }

    // The IDL parser above does not handle generic command arguments. Since the underlying query
    // request requires the following options, manually parse and verify them here.
    if (auto readConcernElt = cmdObj[repl::ReadConcernArgs::kReadConcernFieldName]) {
        if (readConcernElt.type() != BSONType::Object) {
            return Status(ErrorCodes::TypeMismatch,
                          str::stream()
                              << "\"" << repl::ReadConcernArgs::kReadConcernFieldName
                              << "\" had the wrong type. Expected " << typeName(BSONType::Object)
                              << ", found " << typeName(readConcernElt.type()));
        }
        findCommand->setReadConcern(readConcernElt.embeddedObject().getOwned());
    }

    if (auto queryOptionsElt = cmdObj[query_request_helper::kUnwrappedReadPrefField]) {
        if (queryOptionsElt.type() != BSONType::Object) {
            return Status(ErrorCodes::TypeMismatch,
                          str::stream()
                              << "\"" << query_request_helper::kUnwrappedReadPrefField
                              << "\" had the wrong type. Expected " << typeName(BSONType::Object)
                              << ", found " << typeName(queryOptionsElt.type()));
        }
        findCommand->setUnwrappedReadPref(queryOptionsElt.embeddedObject().getOwned());
    }

    if (auto maxTimeMSElt = cmdObj[query_request_helper::cmdOptionMaxTimeMS]) {
        auto maxTimeMS = parseMaxTimeMS(maxTimeMSElt);
        if (!maxTimeMS.isOK()) {
            return maxTimeMS.getStatus();
        }
        findCommand->setMaxTimeMS(static_cast<unsigned int>(maxTimeMS.getValue()));
    }

    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto cq = CanonicalQuery::canonicalize(opCtx,
                                           std::move(findCommand),
                                           isExplain,
                                           expCtx,
                                           extensionsCallback,
                                           MatchExpressionParser::kAllowAllSpecialFeatures);
    if (!cq.isOK()) {
        return cq.getStatus();
    }

    if (cq.getValue()->getFindCommandRequest().getCollation().isEmpty() && defaultCollator) {
        cq.getValue()->setCollator(defaultCollator->clone());
    }

    return ParsedDistinct(std::move(cq.getValue()),
                          parsedDistinct.getKey().toString(),
                          parsedDistinct.getMirrored().value_or(false));
}

}  // namespace mongo
