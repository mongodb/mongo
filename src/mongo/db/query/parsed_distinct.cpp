
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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

const char ParsedDistinct::kKeyField[] = "key";
const char ParsedDistinct::kQueryField[] = "query";
const char ParsedDistinct::kCollationField[] = "collation";
const char ParsedDistinct::kCommentField[] = "comment";

namespace {

/**
 * Helper for when converting a distinct() to an aggregation pipeline. This function will add
 * $unwind stages for each subpath of 'path'.
 *
 * See comments in ParsedDistinct::asAggregationCommand() for more detailed explanation.
 */
void addNestedUnwind(BSONArrayBuilder* pipelineBuilder, const FieldPath& unwindPath) {
    for (size_t i = 0; i < unwindPath.getPathLength(); ++i) {
        StringData pathPrefix = unwindPath.getSubpath(i);
        BSONObjBuilder unwindStageBuilder(pipelineBuilder->subobjStart());
        {
            BSONObjBuilder unwindBuilder(unwindStageBuilder.subobjStart("$unwind"));
            unwindBuilder.append("path", str::stream() << "$" << pathPrefix);
            unwindBuilder.append("preserveNullAndEmptyArrays", true);
        }
        unwindStageBuilder.doneFast();
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
}  // namespace

StatusWith<BSONObj> ParsedDistinct::asAggregationCommand() const {
    BSONObjBuilder aggregationBuilder;

    invariant(_query);
    const QueryRequest& qr = _query->getQueryRequest();
    aggregationBuilder.append("aggregate", qr.nss().coll());

    // Build a pipeline that accomplishes the distinct request. The building code constructs a
    // pipeline that looks like this, assuming the distinct is on the key "a.b.c"
    //
    //      [
    //          { $match: { ... } },
    //          { $unwind: { path: "a", preserveNullAndEmptyArrays: true } },
    //          { $unwind: { path: "a.b", preserveNullAndEmptyArrays: true } },
    //          { $unwind: { path: "a.b.c", preserveNullAndEmptyArrays: true } },
    //          { $match: {"a": {$_internalSchemaType: "object"},
    //                     "a.b": {$_internalSchemaType: "object"}}}
    //          { $group: { _id: null, distinct: { $addToSet: "$<key>" } } }
    //      ]
    //
    // The purpose of the intermediate $unwind stages is to deal with cases where there is an array
    // along the distinct path. For example, if we're distincting on "a.b" and have a document like
    // {a: [{b: 1}, {b: 2}]}, distinct() should produce two values: 1 and 2. If we were to only
    // unwind on "a.b", the document would pass through the $unwind unmodified, and the $group
    // stage would treat the entire array as a key, rather than each element.
    //
    // The reason for the $match with $_internalSchemaType is to deal with cases of nested
    // arrays. The distinct command will not traverse paths inside of nested arrays. For example, a
    // distinct on "a.b" with the following document will produce no results:
    // {a: [[{b: 1}]]
    //
    // Any arrays remaining after the $unwinds must have been nested arrays, so in order to match
    // the behavior of the distinct() command, we filter them out before the $group.
    BSONArrayBuilder pipelineBuilder(aggregationBuilder.subarrayStart("pipeline"));
    if (!qr.getFilter().isEmpty()) {
        BSONObjBuilder matchStageBuilder(pipelineBuilder.subobjStart());
        matchStageBuilder.append("$match", qr.getFilter());
        matchStageBuilder.doneFast();
    }

    FieldPath path(_key);
    addNestedUnwind(&pipelineBuilder, path);
    addMatchRemovingNestedArrays(&pipelineBuilder, path);

    BSONObjBuilder groupStageBuilder(pipelineBuilder.subobjStart());
    {
        BSONObjBuilder groupBuilder(groupStageBuilder.subobjStart("$group"));
        groupBuilder.appendNull("_id");
        {
            BSONObjBuilder distinctBuilder(groupBuilder.subobjStart("distinct"));
            distinctBuilder.append("$addToSet", str::stream() << "$" << _key);
            distinctBuilder.doneFast();
        }
        groupBuilder.doneFast();
    }
    groupStageBuilder.doneFast();
    pipelineBuilder.doneFast();

    aggregationBuilder.append(kCollationField, qr.getCollation());

    if (qr.getMaxTimeMS() > 0) {
        aggregationBuilder.append(QueryRequest::cmdOptionMaxTimeMS, qr.getMaxTimeMS());
    }

    if (!qr.getReadConcern().isEmpty()) {
        aggregationBuilder.append(repl::ReadConcernArgs::kReadConcernFieldName,
                                  qr.getReadConcern());
    }

    if (!qr.getUnwrappedReadPref().isEmpty()) {
        aggregationBuilder.append(QueryRequest::kUnwrappedReadPrefField, qr.getUnwrappedReadPref());
    }

    if (!qr.getComment().empty()) {
        aggregationBuilder.append(kCommentField, qr.getComment());
    }

    // Specify the 'cursor' option so that aggregation uses the cursor interface.
    aggregationBuilder.append("cursor", BSONObj());

    return aggregationBuilder.obj();
}

StatusWith<ParsedDistinct> ParsedDistinct::parse(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 const BSONObj& cmdObj,
                                                 const ExtensionsCallback& extensionsCallback,
                                                 bool isExplain) {
    // Extract the key field.
    BSONElement keyElt;
    auto statusKey = bsonExtractTypedField(cmdObj, kKeyField, BSONType::String, &keyElt);
    if (!statusKey.isOK()) {
        return {statusKey};
    }
    auto key = keyElt.valuestrsafe();

    auto qr = stdx::make_unique<QueryRequest>(nss);

    // Extract the query field. If the query field is nonexistent, an empty query is used.
    if (BSONElement queryElt = cmdObj[kQueryField]) {
        if (queryElt.type() == BSONType::Object) {
            qr->setFilter(queryElt.embeddedObject());
        } else if (queryElt.type() != BSONType::jstNULL) {
            return Status(ErrorCodes::TypeMismatch,
                          str::stream() << "\"" << kQueryField << "\" had the wrong type. Expected "
                                        << typeName(BSONType::Object)
                                        << " or "
                                        << typeName(BSONType::jstNULL)
                                        << ", found "
                                        << typeName(queryElt.type()));
        }
    }

    // Extract the collation field, if it exists.
    if (BSONElement collationElt = cmdObj[kCollationField]) {
        if (collationElt.type() != BSONType::Object) {
            return Status(ErrorCodes::TypeMismatch,
                          str::stream() << "\"" << kCollationField
                                        << "\" had the wrong type. Expected "
                                        << typeName(BSONType::Object)
                                        << ", found "
                                        << typeName(collationElt.type()));
        }
        qr->setCollation(collationElt.embeddedObject());
    }

    if (BSONElement readConcernElt = cmdObj[repl::ReadConcernArgs::kReadConcernFieldName]) {
        if (readConcernElt.type() != BSONType::Object) {
            return Status(ErrorCodes::TypeMismatch,
                          str::stream() << "\"" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << "\" had the wrong type. Expected "
                                        << typeName(BSONType::Object)
                                        << ", found "
                                        << typeName(readConcernElt.type()));
        }
        qr->setReadConcern(readConcernElt.embeddedObject());
    }

    if (BSONElement commentElt = cmdObj[kCommentField]) {
        if (commentElt.type() != BSONType::String) {
            return Status(ErrorCodes::TypeMismatch,
                          str::stream() << "\"" << kCommentField
                                        << "\" had the wrong type. Expected "
                                        << typeName(BSONType::String)
                                        << ", found "
                                        << typeName(commentElt.type()));
        }
        qr->setComment(commentElt.str());
    }

    if (BSONElement queryOptionsElt = cmdObj[QueryRequest::kUnwrappedReadPrefField]) {
        if (queryOptionsElt.type() != BSONType::Object) {
            return Status(ErrorCodes::TypeMismatch,
                          str::stream() << "\"" << QueryRequest::kUnwrappedReadPrefField
                                        << "\" had the wrong type. Expected "
                                        << typeName(BSONType::Object)
                                        << ", found "
                                        << typeName(queryOptionsElt.type()));
        }
        qr->setUnwrappedReadPref(queryOptionsElt.embeddedObject());
    }

    if (BSONElement maxTimeMSElt = cmdObj[QueryRequest::cmdOptionMaxTimeMS]) {
        auto maxTimeMS = QueryRequest::parseMaxTimeMS(maxTimeMSElt);
        if (!maxTimeMS.isOK()) {
            return maxTimeMS.getStatus();
        }
        qr->setMaxTimeMS(static_cast<unsigned int>(maxTimeMS.getValue()));
    }

    qr->setExplain(isExplain);

    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto cq = CanonicalQuery::canonicalize(opCtx,
                                           std::move(qr),
                                           expCtx,
                                           extensionsCallback,
                                           MatchExpressionParser::kAllowAllSpecialFeatures);
    if (!cq.isOK()) {
        return cq.getStatus();
    }

    return ParsedDistinct(std::move(cq.getValue()), std::move(key));
}

}  // namespace mongo
