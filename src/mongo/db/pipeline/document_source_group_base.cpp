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

#include <absl/container/flat_hash_map.h>
#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/accumulator_percentile.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_group_base.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/semantic_analysis.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <cstddef>
#include <memory>

namespace mongo {

Value DocumentSourceGroupBase::serialize(const SerializationOptions& opts) const {
    MutableDocument insides;

    const auto& idFieldNames = _groupProcessor->getIdFieldNames();
    const auto& idExpressions = _groupProcessor->getIdExpressions();
    // Add the _id.
    if (idFieldNames.empty()) {
        invariant(idExpressions.size() == 1);
        insides["_id"] = idExpressions[0]->serialize(opts);
    } else {
        // Decomposed document case.
        invariant(idExpressions.size() == idFieldNames.size());
        MutableDocument md;
        for (size_t i = 0; i < idExpressions.size(); i++) {
            md[opts.serializeFieldPathFromString(idFieldNames[i])] =
                idExpressions[i]->serialize(opts);
        }
        insides["_id"] = md.freezeToValue();
    }

    // Add the remaining fields.
    const auto& accumulatedFields = _groupProcessor->getAccumulationStatements();
    for (auto&& accumulatedField : accumulatedFields) {
        boost::intrusive_ptr<AccumulatorState> accum = accumulatedField.makeAccumulator();
        insides[opts.serializeFieldPathFromString(accumulatedField.fieldName)] =
            Value(accum->serialize(
                accumulatedField.expr.initializer, accumulatedField.expr.argument, opts));
    }

    if (_groupProcessor->doingMerge()) {
        insides[kDoingMergeSpecField] = opts.serializeLiteral(true);
    } else if (getExpCtx()->isFeatureFlagShardFilteringDistinctScanEnabled() &&
               !_groupProcessor->willBeMerged() && opts.isKeepingLiteralsUnchanged() &&
               !opts.serializeForQueryAnalysis) {
        // Only serialize this flag when it is set to false & we are not already merging & this is
        // not being used for query settings & this is not being rewritten for FLE- otherwise,
        // mongod must infer from the expression context what to do.
        insides[kWillBeMergedSpecField] = opts.serializeLiteral(_groupProcessor->willBeMerged());
    }

    serializeAdditionalFields(insides, opts);

    MutableDocument out;
    out[getSourceName()] = insides.freezeToValue();

    if (opts.isSerializingForExplain() &&
        *opts.verbosity >= ExplainOptions::Verbosity::kExecStats) {
        MutableDocument md;

        const auto& memoryTracker = _groupProcessor->getMemoryTracker();
        for (size_t i = 0; i < accumulatedFields.size(); i++) {
            md[opts.serializeFieldPathFromString(accumulatedFields[i].fieldName)] =
                opts.serializeLiteral(static_cast<long long>(
                    memoryTracker.peakTrackedMemoryBytes(accumulatedFields[i].fieldName)));
        }

        out["maxAccumulatorMemoryUsageBytes"] = Value(md.freezeToValue());

        const auto& stats = _groupProcessor->getStats();
        out["totalOutputDataSizeBytes"] =
            opts.serializeLiteral(static_cast<long long>(stats.totalOutputDataSizeBytes));
        out["usedDisk"] = opts.serializeLiteral(stats.spillingStats.getSpills() > 0);
        out["spills"] =
            opts.serializeLiteral(static_cast<long long>(stats.spillingStats.getSpills()));
        out["spilledDataStorageSize"] = opts.serializeLiteral(
            static_cast<long long>(stats.spillingStats.getSpilledDataStorageSize()));
        out["spilledBytes"] =
            opts.serializeLiteral(static_cast<long long>(stats.spillingStats.getSpilledBytes()));
        out["spilledRecords"] =
            opts.serializeLiteral(static_cast<long long>(stats.spillingStats.getSpilledRecords()));
        if (feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled()) {
            out["peakTrackedMemBytes"] =
                opts.serializeLiteral(static_cast<long long>(stats.peakTrackedMemBytes));
        }
    }

    return out.freezeToValue();
}

boost::intrusive_ptr<DocumentSource> DocumentSourceGroupBase::optimize() {
    // Optimizing a 'DocumentSourceGroupBase' might modify its expressions to become incompatible
    // with SBE. We temporarily highjack the context's 'sbeCompatible' flag to communicate the
    // situation back to the 'DocumentSourceGroupBase'. Notice, that while a particular
    // 'DocumentSourceGroupBase' might become incompatible with SBE, other groups in the pipeline
    // and the collection access could be still eligible for lowering to SBE, thus we must reset the
    // context's 'sbeCompatible' flag back to its original value at the end of the 'optimize()'
    // call.
    auto& idExpressions = _groupProcessor->getMutableIdExpressions();
    auto expCtx = idExpressions[0]->getExpressionContext();
    auto origSbeCompatibility = expCtx->getSbeCompatibility();
    expCtx->setSbeCompatibility(SbeCompatibility::noRequirements);

    // TODO: If all idExpressions are ExpressionConstants after optimization, then we know
    // there will be only one group. We should take advantage of that to avoid going through the
    // hash table.
    for (size_t i = 0; i < idExpressions.size(); i++) {
        idExpressions[i] = idExpressions[i]->optimize();
    }

    auto& accumulatedFields = _groupProcessor->getMutableAccumulationStatements();
    for (auto& accumulatedField : accumulatedFields) {
        accumulatedField.expr.initializer = accumulatedField.expr.initializer->optimize();
        accumulatedField.expr.argument = accumulatedField.expr.argument->optimize();
    }

    _sbeCompatibility = std::min(_sbeCompatibility, expCtx->getSbeCompatibility());
    expCtx->setSbeCompatibility(origSbeCompatibility);

    return this;
}

DepsTracker::State DocumentSourceGroupBase::getDependencies(DepsTracker* deps) const {
    // add the _id
    const auto& idExpressions = _groupProcessor->getIdExpressions();
    for (size_t i = 0; i < idExpressions.size(); i++) {
        expression::addDependencies(idExpressions[i].get(), deps);
    }

    // add the rest
    const auto& accumulatedFields = _groupProcessor->getAccumulationStatements();
    for (auto&& accumulatedField : accumulatedFields) {
        expression::addDependencies(accumulatedField.expr.argument.get(), deps);
        // Don't add initializer, because it doesn't refer to docs from the input stream.
    }

    return DepsTracker::State::EXHAUSTIVE_ALL;
}

void DocumentSourceGroupBase::addVariableRefs(std::set<Variables::Id>* refs) const {
    const auto& idExpressions = _groupProcessor->getIdExpressions();
    for (const auto& idExpr : idExpressions) {
        expression::addVariableRefs(idExpr.get(), refs);
    }

    const auto& accumulatedFields = _groupProcessor->getAccumulationStatements();
    for (auto&& accumulatedField : accumulatedFields) {
        expression::addVariableRefs(accumulatedField.expr.argument.get(), refs);
    }
}

DocumentSource::GetModPathsReturn DocumentSourceGroupBase::getModifiedPaths() const {
    // We preserve none of the fields, but any fields referenced as part of the group key are
    // logically just renamed.
    StringMap<std::string> renames;
    const auto& idFieldNames = _groupProcessor->getIdFieldNames();
    const auto& idExpressions = _groupProcessor->getIdExpressions();
    for (std::size_t i = 0; i < idExpressions.size(); ++i) {
        auto idExp = idExpressions[i];
        auto pathToPutResultOfExpression = idFieldNames.empty() ? "_id" : "_id." + idFieldNames[i];
        auto computedPaths = idExp->getComputedPaths(pathToPutResultOfExpression);
        for (auto&& rename : computedPaths.renames) {
            renames[rename.first] = rename.second;
        }
    }

    return {DocumentSource::GetModPathsReturn::Type::kAllExcept,
            OrderedPathSet{},  // No fields are preserved.
            std::move(renames)};
}

StringMap<boost::intrusive_ptr<Expression>> DocumentSourceGroupBase::getIdFields() const {
    const auto& idFieldNames = _groupProcessor->getIdFieldNames();
    const auto& idExpressions = _groupProcessor->getIdExpressions();
    if (idFieldNames.empty()) {
        invariant(idExpressions.size() == 1);
        return {{"_id", idExpressions[0]}};
    } else {
        invariant(idFieldNames.size() == idExpressions.size());
        StringMap<boost::intrusive_ptr<Expression>> result;
        for (std::size_t i = 0; i < idFieldNames.size(); ++i) {
            result["_id." + idFieldNames[i]] = idExpressions[i];
        }
        return result;
    }
}

std::vector<boost::intrusive_ptr<Expression>>& DocumentSourceGroupBase::getMutableIdFields() {
    return _groupProcessor->getMutableIdExpressions();
}

const std::vector<AccumulationStatement>& DocumentSourceGroupBase::getAccumulationStatements()
    const {
    return _groupProcessor->getAccumulationStatements();
}

std::vector<AccumulationStatement>& DocumentSourceGroupBase::getMutableAccumulationStatements() {
    return _groupProcessor->getMutableAccumulationStatements();
}

DocumentSourceGroupBase::DocumentSourceGroupBase(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<int64_t> maxMemoryUsageBytes)
    : DocumentSource(stageName, expCtx),
      _groupProcessor(std::make_shared<GroupProcessor>(
          expCtx,
          maxMemoryUsageBytes
              ? *maxMemoryUsageBytes
              : loadMemoryLimit(StageMemoryLimit::DocumentSourceGroupMaxMemoryBytes))),
      _sbeCompatibility(SbeCompatibility::notCompatible) {}

namespace {

boost::intrusive_ptr<Expression> parseIdExpression(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement groupField,
    const VariablesParseState& vps) {
    if (groupField.type() == BSONType::object) {
        // {_id: {}} is treated as grouping on a constant, not an expression
        if (groupField.Obj().isEmpty()) {
            return ExpressionConstant::create(expCtx.get(), Value(groupField));
        }

        const BSONObj idKeyObj = groupField.Obj();
        if (idKeyObj.firstElementFieldName()[0] == '$') {
            // grouping on a $op expression
            return Expression::parseObject(expCtx.get(), idKeyObj, vps);
        } else {
            for (auto&& field : idKeyObj) {
                uassert(17390,
                        "$group does not support inclusion-style expressions",
                        !field.isNumber() && field.type() != BSONType::boolean);
            }
            return ExpressionObject::parse(expCtx.get(), idKeyObj, vps);
        }
    } else {
        return Expression::parseOperand(expCtx.get(), groupField, vps);
    }
}

}  // namespace

boost::intrusive_ptr<Expression> DocumentSourceGroupBase::getIdExpression() const {
    return _groupProcessor->getIdExpression();
}

void DocumentSourceGroupBase::initializeFromBson(BSONElement elem) {
    uassert(
        15947, "a group's fields must be specified in an object", elem.type() == BSONType::object);

    const auto& idExpressions = _groupProcessor->getIdExpressions();
    BSONObj groupObj(elem.Obj());
    BSONObjIterator groupIterator(groupObj);
    VariablesParseState vps = getExpCtx()->variablesParseState;
    getExpCtx()->setSbeGroupCompatibility(SbeCompatibility::noRequirements);

    // If 'kWillBeMergedSpecField' is missing, we will infer whether or not this stage needs to be
    // merged based on the expression context.
    _groupProcessor->setWillBeMerged(getExpCtx()->getNeedsMerge());

    while (groupIterator.more()) {
        BSONElement groupField(groupIterator.next());
        StringData pFieldName = groupField.fieldNameStringData();
        if (pFieldName == "_id") {
            uassert(15948, "a group's _id may only be specified once", idExpressions.empty());
            _groupProcessor->setIdExpression(parseIdExpression(getExpCtx(), groupField, vps));
            invariant(!idExpressions.empty());
        } else if (pFieldName == kDoingMergeSpecField) {
            uassert(ErrorCodes::Unauthorized,
                    "Setting '$doingMerge' is not allowed in user requests",
                    AuthorizationSession::get(getExpCtx()->getOperationContext()->getClient())
                        ->isAuthorizedForClusterAction(ActionType::internal, boost::none));

            massert(17030, "$doingMerge should be true if present", groupField.Bool());

            _groupProcessor->setDoingMerge(true);
        } else if (pFieldName == kWillBeMergedSpecField) {
            // If mongos sets this field, we should always use it regardless of feature flag / FCV,
            // since mongos already decided what the merging pipeline needs to do.
            _groupProcessor->setWillBeMerged(groupField.Bool());

        } else if (isSpecFieldReserved(pFieldName)) {
            // No-op: field is used by the derived class.
        } else {
            // Any other field will be treated as an accumulator specification.
            _groupProcessor->addAccumulationStatement(
                AccumulationStatement::parseAccumulationStatement(
                    getExpCtx().get(), groupField, vps));
        }
    }
    _sbeCompatibility =
        std::min(getExpCtx()->getSbeGroupCompatibility(), getExpCtx()->getSbeCompatibility());

    uassert(15955, "a group specification must include an _id", !idExpressions.empty());
}

bool DocumentSourceGroupBase::pathIncludedInGroupKeys(const std::string& dottedPath) const {
    const auto& idExpressions = _groupProcessor->getIdExpressions();
    return std::any_of(idExpressions.begin(), idExpressions.end(), [&dottedPath](const auto& exp) {
        if (auto fieldExp = dynamic_cast<ExpressionFieldPath*>(exp.get())) {
            if (fieldExp->representsPath(dottedPath)) {
                return true;
            }
        }
        return false;
    });
}

/**
 * Visitor collecting paths which are referenced in an object or an array,
 * but _not_ in any expression which may compute a derived value.
 *
 * e.g., {a:"$a", b:"$b"} and ["$a", "$b"] both reference "a" and "b".
 *
 * For more complex expressions, like
 *  {a:{"$add":["$b", "$c"]}, d:"$d"}
 * only the "unmodified" fields will be reported, "d".
 *
 * This can be used to determine fields a group _id "directly" references.
 */
class TriviallyReferencedFieldsVisitor : public SelectiveConstExpressionVisitorBase {
public:
    using SelectiveConstExpressionVisitorBase::visit;
    void visitChildren(const Expression* expr) {
        for (const auto& child : expr->getChildren()) {
            child->acceptVisitor(this);
        }
    }
    void visit(const ExpressionArray* expr) override {
        visitChildren(expr);
    }
    void visit(const ExpressionObject* expr) override {
        visitChildren(expr);
    }
    void visit(const ExpressionFieldPath* efp) override {
        // Note: we can't infer used fields from $$ROOT.
        if (!efp->isVariableReference() && !efp->isROOT()) {
            fields.insert(efp->getFieldPathWithoutCurrentPrefix().fullPath());
        }
    }
    OrderedPathSet fields;
};

OrderedPathSet DocumentSourceGroupBase::getTriviallyReferencedPaths() const {
    TriviallyReferencedFieldsVisitor visitor;
    for (const auto& expr : _groupProcessor->getIdExpressions()) {
        expr->acceptVisitor(&visitor);
    }
    return visitor.fields;
}

bool DocumentSourceGroupBase::canRunInParallelBeforeWriteStage(
    const OrderedPathSet& nameOfShardKeyFieldsUponEntryToStage) const {
    if (_groupProcessor->doingMerge()) {
        return true;  // This is fine.
    }

    // Certain $group stages are allowed to execute on each exchange consumer. In order to
    // guarantee each consumer will only group together data from its own shard, the $group must
    // group on a superset of the shard key.
    for (auto&& currentPathOfShardKey : nameOfShardKeyFieldsUponEntryToStage) {
        if (!pathIncludedInGroupKeys(currentPathOfShardKey)) {
            // This requires an exact path match, but as a future optimization certain path
            // prefixes should be okay. For example, if the shard key path is "a.b", and we're
            // grouping by "a", then each group of "a" is strictly more specific than "a.b", so
            // we can deduce that grouping by "a" will not need to group together documents
            // across different values of the shard key field "a.b", and thus as long as any
            // other shard key fields are similarly preserved will not need to consume a merged
            // stream to perform the group.
            return false;
        }
    }
    return true;
}

namespace {

/**
 * Helper to check if all accumulated fields need the same first or last document from the input for
 * the output. Also checks whether the sort pattern is same across all $top and $bottom. This
 * returns {kFirstInputDocument, none} for no accumulators.
 *
 * If all accumulators need the same first or last doc, this returns a pair of:
 * - The same AccumulatorDocumentsNeeded across all accumulators.
 * - an optional SortPattern when the needed document is either kFirstOutputDocument or
 *   kLastOutputDocument.
 */
using DocNeededAndSortPattern = std::pair<AccumulatorDocumentsNeeded, boost::optional<SortPattern>>;

boost::optional<DocNeededAndSortPattern> allAccsNeedFirstOrLastDoc(
    const std::vector<AccumulationStatement>& accumulatedFields) {
    if (accumulatedFields.empty()) {
        return DocNeededAndSortPattern{AccumulatorDocumentsNeeded::kFirstInputDocument,
                                       boost::none};
    }

    auto it = accumulatedFields.begin();
    // Unfortunately, the docs needed and sort pattern can be extracted only from 'AccumulatorState'
    // object and so we need to create one using the factory.
    const auto accState = it->makeAccumulator();
    const auto& docNeeded = accState->documentsNeeded();

    if (docNeeded == AccumulatorDocumentsNeeded::kAllDocuments) {
        return boost::none;
    }

    boost::optional<SortPattern> sortPattern =
        (docNeeded == AccumulatorDocumentsNeeded::kFirstOutputDocument ||
         docNeeded == AccumulatorDocumentsNeeded::kLastOutputDocument)
        ? getAccSortPattern(accState)
        : static_cast<boost::optional<SortPattern>>(boost::none);
    bool allAccsNeedSameDoc = std::all_of(++it, accumulatedFields.end(), [&](auto&& accumulator) {
        const auto accState = accumulator.makeAccumulator();
        const auto& doc = accState->documentsNeeded();
        // To be eligible for DISTINCT_SCAN plan work, the sort pattern should be same across all
        // $top and $bottoms.
        return doc == docNeeded && (!sortPattern || *sortPattern == getAccSortPattern(accState));
    });
    if (!allAccsNeedSameDoc) {
        return boost::none;
    }

    return DocNeededAndSortPattern{docNeeded, sortPattern};
}

}  // namespace

auto DocumentSourceGroupBase::getRewriteGroupRequirements() const
    -> boost::optional<RewriteGroupRequirements> {
    const auto& idExpressions = _groupProcessor->getIdExpressions();
    if (idExpressions.size() != 1) {
        // This transformation is only intended for $group stages that group on a single field.
        return boost::none;
    }

    auto fieldPathExpr = dynamic_cast<ExpressionFieldPath*>(idExpressions.front().get());
    if (!fieldPathExpr || fieldPathExpr->isVariableReference()) {
        return boost::none;
    }

    const auto fieldPath = fieldPathExpr->getFieldPath();
    if (fieldPath.getPathLength() == 1) {
        // The path is $$CURRENT or $$ROOT. This isn't really a sensible value to group by (since
        // each document has a unique _id, it will just return the entire collection). We only
        // apply the rewrite when grouping by a single field, so we cannot apply it in this case,
        // where we are grouping by the entire document.
        tassert(5943200,
                "Optimization attempted on group by always-dissimilar system variable",
                fieldPath.getFieldName(0) == "CURRENT" || fieldPath.getFieldName(0) == "ROOT");
        return boost::none;
    }

    // We do this transformation only if there are all $first, all $last, all $top, all $bottom, or
    // no accumulators.
    const auto& accumulatedFields = _groupProcessor->getAccumulationStatements();
    auto docNeededAndSortPattern = allAccsNeedFirstOrLastDoc(accumulatedFields);
    if (!docNeededAndSortPattern) {
        return boost::none;
    }

    auto [docNeeded, sortPattern] = *docNeededAndSortPattern;
    auto groupId = fieldPath.tail().fullPath();
    return RewriteGroupRequirements{docNeeded, groupId, sortPattern};
}

std::pair<boost::optional<SortPattern>, std::unique_ptr<GroupFromFirstDocumentTransformation>>
DocumentSourceGroupBase::rewriteGroupAsTransformOnFirstDocument() const {
    auto rewriteGroupRequirements = getRewriteGroupRequirements();
    if (!rewriteGroupRequirements) {
        return {boost::none, nullptr};
    }

    auto [docsNeeded, groupId, sortPattern] = *rewriteGroupRequirements;

    boost::intrusive_ptr<Expression> idField;
    const auto& idFieldNames = _groupProcessor->getIdFieldNames();
    // The _id field can be specified either as a fieldpath (ex. _id: "$a") or as a singleton
    // object (ex. _id: {v: "$a"}).
    if (idFieldNames.empty()) {
        idField = ExpressionFieldPath::createPathFromString(
            getExpCtx().get(), groupId, getExpCtx()->variablesParseState);
    } else {
        invariant(idFieldNames.size() == 1);
        idField = ExpressionObject::create(
            getExpCtx().get(),
            {{idFieldNames.front(), _groupProcessor->getIdExpressions().front()}});
    }

    std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> fields{
        {std::pair{"_id", idField}}};
    for (auto&& accumulator : _groupProcessor->getAccumulationStatements()) {
        switch (docsNeeded) {
            case AccumulatorDocumentsNeeded::kFirstInputDocument:
            case AccumulatorDocumentsNeeded::kLastInputDocument:
                fields.emplace_back(accumulator.fieldName, accumulator.expr.argument);
                break;
            case AccumulatorDocumentsNeeded::kFirstOutputDocument:
            case AccumulatorDocumentsNeeded::kLastOutputDocument:
                // We only want to add the 'output' portion of the accumulator expression since
                // that's the only part that should be accumulated. We know 'output' is the
                // first child of $top and $bottom accumulators since it is added first in
                // AccumulatorTopBottomN<sense, single>::parseTopBottomN().
                tassert(9657901,
                        str::stream() << "'" << accumulator.fieldName << "' must not be empty",
                        !accumulator.expr.argument->getChildren().empty());
                fields.emplace_back(accumulator.fieldName,
                                    accumulator.expr.argument->getChildren()[0]);
                break;
            default:
                return {boost::none, nullptr};
        }
    }

    return {sortPattern,
            GroupFromFirstDocumentTransformation::create(
                getExpCtx(), groupId, getSourceName(), std::move(fields), docsNeeded)};
}

/**
 * Verify if the current $group is appended to `pipeline`, it would group on a superset of the
 * shard key. This considers any renames which occur in the pipeline.
 */
bool DocumentSourceGroupBase::groupIsOnShardKey(
    const Pipeline& pipeline, const boost::optional<OrderedPathSet>& initialShardKeyPaths) const {
    if (!initialShardKeyPaths) {
        return false;
    }

    const auto& shardKeyPaths = *initialShardKeyPaths;

    const auto groupExprs = getTriviallyReferencedPaths();
    if (groupExprs.empty()) {
        // The group _id doesn't contain any simple referenced paths; it may be
        // a constant, or a more complex computation. In any case, it is not
        // guaranteed that the group can be pushed down as it doesn't directly
        // contain the shard key.
        return false;
    }

    const auto& stages = pipeline.getSources();
    // Walk backwards through the pipeline to find the original paths for
    // these fields, before any renames.
    const auto originPaths = semantic_analysis::traceOriginatingPaths(stages, groupExprs);

    if (originPaths.empty()) {
        // The group _id does not include fields which are derived from the shard key purely by
        // rename/projection. Even if the entire shard key is used to compute the _id, that alone is
        // not sufficient to guarantee the _id will uniquely occur on one shard.
        return false;
    }

    if (!std::includes(originPaths.begin(),
                       originPaths.end(),
                       shardKeyPaths.begin(),
                       shardKeyPaths.end(),
                       originPaths.key_comp())) {
        // The group does not include all of the paths of the shard key; it cannot be
        // pushed down.
        return false;
    }
    return true;
}

boost::optional<DocumentSource::DistributedPlanLogic>
DocumentSourceGroupBase::pipelineDependentDistributedPlanLogic(
    const DocumentSourceGroup::DistributedPlanContext& ctx) {
    if (!getExpCtx()->isFeatureFlagShardFilteringDistinctScanEnabled()) {
        // Feature flag guards ability to entirely push down a $group; if disabled
        // do not perform any pipeline aware logic.
        return distributedPlanLogic();
    }

    if (!CollatorInterface::isSimpleCollator(getExpCtx()->getCollator())) {
        // A collation on the aggregation may result in the aggregation being more coarse-grained
        // than the shard-key, i.e. pushing the $group down fully may result in more group keys than
        // we actually want.
        return distributedPlanLogic();
    }

    if (repl::ReadConcernArgs::get(getExpCtx()->getOperationContext()).getLevel() ==
        repl::ReadConcernLevel::kAvailableReadConcern) {
        // Can't rely on multiple shards not returning the same document twice.
        return distributedPlanLogic();
    }

    // TODO SERVER-97135: Refactor so we can remove the following check.
    auto mergeStage = ctx.pipelineSuffix.empty()
        ? nullptr
        : dynamic_cast<DocumentSourceMerge*>(ctx.pipelineSuffix.getSources().back().get());
    if (mergeStage) {
        // This $group may be eligible for a $exchange optimisation, which fully pushing down $group
        // would prevent.
        return distributedPlanLogic();
    }

    if (getExpCtx()->getSubPipelineDepth() >= 1) {
        // TODO SERVER-99094: Allow $group pushdown within nested pipelines.
        return distributedPlanLogic();
    }

    if (groupIsOnShardKey(ctx.pipelinePrefix, ctx.shardKeyPaths)) {
        // This group can fully execute on a shard, because no two shards will return the same group
        // key. Prior calls to distributedPlanLogic() may have set the 'willBeMerged' flag to true,
        // so we set it to false here to ensure it is correct.
        _groupProcessor->setWillBeMerged(false);
        return boost::none;
    }
    // Fall back to non-pipeline dependent.
    return distributedPlanLogic();
}

boost::optional<DocumentSource::DistributedPlanLogic>
DocumentSourceGroupBase::distributedPlanLogic() {
    VariablesParseState vps = getExpCtx()->variablesParseState;
    /* the merger will use the same grouping key */
    auto mergerGroupByExpression = ExpressionFieldPath::parse(getExpCtx().get(), "$$ROOT._id", vps);

    auto clone = this->clone(getExpCtx());

    std::vector<AccumulationStatement> mergerAccumulators;
    const auto& accumulatedFields = _groupProcessor->getAccumulationStatements();
    mergerAccumulators.reserve(accumulatedFields.size());
    for (auto&& accumulatedField : accumulatedFields) {
        // The merger's output field names will be the same, as will the accumulator factories.
        // However, for some accumulators, the expression to be accumulated will be different. The
        // original accumulator may be collecting an expression based on a field expression or
        // constant.  Here, we accumulate the output of the same name from the prior group.
        auto copiedAccumulatedField = accumulatedField;
        copiedAccumulatedField.expr.argument = ExpressionFieldPath::parse(
            getExpCtx().get(), "$$ROOT." + copiedAccumulatedField.fieldName, vps);

        // For the accurate (discrete and continuous) $percentile and $median accumulators we cannot
        // push down computation to be done in parallel on the shards.  The presence of an accurate
        // percentile in a $group prevents the entire $group from being pushed down.
        if (copiedAccumulatedField.expr.name == "$percentile" ||
            copiedAccumulatedField.expr.name == "$median") {
            auto accumState = copiedAccumulatedField.expr.factory();
            auto accumPercentile = dynamic_cast<AccumulatorPercentile*>(accumState.get());
            tassert(9158201,
                    "casting AccumulatorState* to AccumulatorPercentile* failed",
                    accumPercentile);
            static_cast<DocumentSourceGroup*>(clone.get())->_groupProcessor->setWillBeMerged(false);
            if (accumPercentile->getMethod() != PercentileMethodEnum::kApproximate) {
                return DistributedPlanLogic{nullptr, std::move(clone), boost::none};
            }
        }
        mergerAccumulators.emplace_back(std::move(copiedAccumulatedField));
    }

    // When merging, we always use generic hash based algorithm.
    boost::intrusive_ptr<DocumentSourceGroup> mergingGroup = DocumentSourceGroup::create(
        getExpCtx(), std::move(mergerGroupByExpression), std::move(mergerAccumulators), false);
    mergingGroup->_groupProcessor->setDoingMerge(true);

    static_cast<DocumentSourceGroup*>(clone.get())->_groupProcessor->setWillBeMerged(true);

    // {shardsStage, mergingStage, sortPattern}
    return DistributedPlanLogic{std::move(clone), mergingGroup, boost::none};
}

}  // namespace mongo
