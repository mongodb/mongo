/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/db/pipeline/document_source_change_stream_unwind_transaction.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/change_stream_filter_helpers.h"
#include "mongo/db/pipeline/change_stream_rewrite_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>
#include <list>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalChangeStreamUnwindTransaction,
                                  LiteParsedDocumentSourceChangeStreamInternal::parse,
                                  DocumentSourceChangeStreamUnwindTransaction::createFromBson,
                                  true);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamUnwindTransaction,
                            DocumentSourceChangeStreamUnwindTransaction::id)

namespace {
const std::set<std::string> kUnwindExcludedFields = {"clusterTime", "lsid", "txnNumber"};
}

namespace change_stream_filter {
/**
 * Build a filter, similar to the optimized oplog filter, designed to reject individual transaction
 * entries that we know would eventually get rejected by the 'userMatch' filter if they continued
 * through the rest of the pipeline. We must also adjust the filter slightly for user rewrites, as
 * events within a transaction do not have certain fields that are common to other oplog entries.
 *
 * NB: The new filter may contain references to strings in the BSONObj that 'userMatch' originated
 * from. Callers that keep the new filter long-term should serialize and re-parse it to guard
 * against the possibility of stale string references.
 */
std::unique_ptr<MatchExpression> buildUnwindTransactionFilter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const MatchExpression* userMatch,
    std::vector<BSONObj>& bsonObj) {
    // The transaction unwind filter is the same as the operation filter applied to the oplog. This
    // includes a namespace filter, which ensures that it will discard all documents that would be
    // filtered out by the default 'ns' filter this stage gets initialized with.
    auto unwindFilter =
        std::make_unique<AndMatchExpression>(buildOperationFilter(expCtx, nullptr, bsonObj));

    // To correctly handle filtering out entries of direct write operations on orphaned documents,
    // we include a filter for "fromMigrate" flagged operations, unless "fromMigrate" events are
    // explicitly requested in the spec.
    if (!expCtx->getChangeStreamSpec()->getShowMigrationEvents()) {
        unwindFilter->add(buildNotFromMigrateFilter(expCtx, userMatch, bsonObj));
    }

    // Attempt to rewrite the user's filter and combine it with the standard operation filter. We do
    // this separately because we need to exclude certain fields from the user's filters. Unwound
    // transaction events do not have these fields until we populate them from the commitTransaction
    // event. We already applied these predicates during the oplog scan, so we know that they match.
    if (auto rewrittenMatch = change_stream_rewrite::rewriteFilterForFields(
            expCtx, userMatch, bsonObj, {}, kUnwindExcludedFields)) {
        unwindFilter->add(std::move(rewrittenMatch));
    }
    return optimizeMatchExpression(std::move(unwindFilter));
}
}  // namespace change_stream_filter

boost::intrusive_ptr<DocumentSourceChangeStreamUnwindTransaction>
DocumentSourceChangeStreamUnwindTransaction::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::vector<BSONObj> bsonObj = std::vector<BSONObj>{};
    std::unique_ptr<MatchExpression> matchExpr =
        change_stream_filter::buildUnwindTransactionFilter(expCtx, nullptr, bsonObj);
    return new DocumentSourceChangeStreamUnwindTransaction(matchExpr->serialize(), expCtx);
}

boost::intrusive_ptr<DocumentSourceChangeStreamUnwindTransaction>
DocumentSourceChangeStreamUnwindTransaction::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5467605,
            str::stream() << "the '" << kStageName << "' stage spec must be an object",
            elem.type() == BSONType::object);
    auto parsedSpec = DocumentSourceChangeStreamUnwindTransactionSpec::parse(
        elem.Obj(), IDLParserContext("DocumentSourceChangeStreamUnwindTransactionSpec"));
    return new DocumentSourceChangeStreamUnwindTransaction(parsedSpec.getFilter(), expCtx);
}

DocumentSourceChangeStreamUnwindTransaction::DocumentSourceChangeStreamUnwindTransaction(
    BSONObj filter, const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceInternalChangeStreamStage(kStageName, expCtx) {
    rebuild(std::move(filter));
}

void DocumentSourceChangeStreamUnwindTransaction::rebuild(BSONObj filter) {
    _filter = filter.getOwned();
    _expression = MatchExpressionParser::parseAndNormalize(filter, getExpCtx());
}

StageConstraints DocumentSourceChangeStreamUnwindTransaction::constraints(
    PipelineSplitState pipeState) const {
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 HostTypeRequirement::kNone,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kChangeStreamStage);
    constraints.consumesLogicalCollectionData = false;
    return constraints;
}

Value DocumentSourceChangeStreamUnwindTransaction::doSerialize(
    const SerializationOptions& opts) const {
    tassert(7481400, "expression has not been initialized", _expression);

    if (opts.isSerializingForExplain()) {
        BSONObjBuilder builder;
        builder.append("stage"_sd, "internalUnwindTransaction"_sd);
        builder.append(DocumentSourceChangeStreamUnwindTransactionSpec::kFilterFieldName,
                       _expression->serialize(opts));

        return Value(DOC(DocumentSourceChangeStream::kStageName << builder.obj()));
    }

    // 'SerializationOptions' are not required here, since serialization for explain and query
    // stats occur before this function call.
    return Value(Document{
        {kStageName, Value{DocumentSourceChangeStreamUnwindTransactionSpec{_filter}.toBSON()}}});
}

DepsTracker::State DocumentSourceChangeStreamUnwindTransaction::getDependencies(
    DepsTracker* deps) const {
    deps->fields.insert(std::string{repl::OplogEntry::kOpTypeFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kTimestampFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kObjectFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kSessionIdFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kTermFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kTxnNumberFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kWallClockTimeFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kMultiOpTypeFieldName});

    return DepsTracker::State::SEE_NEXT;
}

DocumentSource::GetModPathsReturn DocumentSourceChangeStreamUnwindTransaction::getModifiedPaths()
    const {
    return {DocumentSource::GetModPathsReturn::Type::kAllPaths, OrderedPathSet{}, {}};
}

DocumentSourceContainer::iterator DocumentSourceChangeStreamUnwindTransaction::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    tassert(5687205, "Iterator mismatch during optimization", *itr == this);

    auto nextChangeStreamStageItr = std::next(itr);

    // The additional filtering added by this optimization may incorrectly filter out events if it
    // runs with the non-simple collation.
    if (getExpCtx()->getCollator()) {
        return nextChangeStreamStageItr;
    }

    // Seek to the stage that immediately follows the change streams stages.
    itr = std::find_if_not(itr, container->end(), [](const auto& stage) {
        return stage->constraints().isChangeStreamStage();
    });

    if (itr == container->end()) {
        // This pipeline is just the change stream.
        return itr;
    }

    auto matchStage = dynamic_cast<DocumentSourceMatch*>(itr->get());
    if (!matchStage) {
        // This function only attempts to optimize a $match that immediately follows expanded
        // $changeStream stages. That does not apply here, and we resume optimization at the last
        // change stream stage, in case a "swap" optimization can apply between it and the stage
        // that follows it. For example, $project stages can swap in front of the last change stream
        // stages.
        return std::prev(itr);
    }

    // Build a filter which discards unwound transaction operations which would have been filtered
    // out later in the pipeline by the user's $match filter.
    std::vector<BSONObj> bsonObj = std::vector<BSONObj>{};
    auto unwindTransactionFilter = change_stream_filter::buildUnwindTransactionFilter(
        getExpCtx(), matchStage->getMatchExpression(), bsonObj);

    // Replace the default filter with the new, more restrictive one. Note that the resulting
    // expression must be serialized then deserialized to ensure it does not retain unsafe
    // references to strings in the 'matchStage' filter.
    rebuild(unwindTransactionFilter->serialize());

    // Continue optimization at the next change stream stage.
    return nextChangeStreamStageItr;
}
}  // namespace mongo
