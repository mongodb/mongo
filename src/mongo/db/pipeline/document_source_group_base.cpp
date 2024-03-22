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
#include <algorithm>
#include <cstddef>
#include <memory>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_group_base.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_dependencies.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

namespace {

/**
 * Helper to check if all accumulated fields need the same document.
 */
bool accsNeedSameDoc(const std::vector<AccumulationStatement>& accumulatedFields,
                     AccumulatorDocumentsNeeded docNeeded) {
    return std::all_of(accumulatedFields.begin(), accumulatedFields.end(), [&](auto&& accumulator) {
        const auto& doc = accumulator.makeAccumulator()->documentsNeeded();
        return doc == docNeeded;
    });
}

}  // namespace

DocumentSourceGroupBase::~DocumentSourceGroupBase() {
    const auto& stats = _groupProcessor.getStats();
    groupCounters.incrementGroupCounters(
        stats.spills, stats.spilledDataStorageSize, stats.spilledRecords);
}

Value DocumentSourceGroupBase::serialize(const SerializationOptions& opts) const {
    MutableDocument insides;

    const auto& idFieldNames = _groupProcessor.getIdFieldNames();
    const auto& idExpressions = _groupProcessor.getIdExpressions();
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
    const auto& accumulatedFields = _groupProcessor.getAccumulationStatements();
    for (auto&& accumulatedField : accumulatedFields) {
        boost::intrusive_ptr<AccumulatorState> accum = accumulatedField.makeAccumulator();
        insides[opts.serializeFieldPathFromString(accumulatedField.fieldName)] =
            Value(accum->serialize(
                accumulatedField.expr.initializer, accumulatedField.expr.argument, opts));
    }

    if (_groupProcessor.doingMerge()) {
        insides["$doingMerge"] = opts.serializeLiteral(true);
    }

    serializeAdditionalFields(insides, opts);

    MutableDocument out;
    out[getSourceName()] = insides.freezeToValue();

    if (opts.verbosity && *opts.verbosity >= ExplainOptions::Verbosity::kExecStats) {
        MutableDocument md;

        const auto& memoryTracker = _groupProcessor.getMemoryTracker();
        for (size_t i = 0; i < accumulatedFields.size(); i++) {
            md[opts.serializeFieldPathFromString(accumulatedFields[i].fieldName)] =
                opts.serializeLiteral(static_cast<long long>(
                    memoryTracker[accumulatedFields[i].fieldName].maxMemoryBytes()));
        }

        out["maxAccumulatorMemoryUsageBytes"] = Value(md.freezeToValue());

        const auto& stats = _groupProcessor.getStats();
        out["totalOutputDataSizeBytes"] =
            opts.serializeLiteral(static_cast<long long>(stats.totalOutputDataSizeBytes));
        out["usedDisk"] = opts.serializeLiteral(stats.spills > 0);
        out["spills"] = opts.serializeLiteral(static_cast<long long>(stats.spills));
        out["spilledDataStorageSize"] =
            opts.serializeLiteral(static_cast<long long>(stats.spilledDataStorageSize));
        out["numBytesSpilledEstimate"] =
            opts.serializeLiteral(static_cast<long long>(stats.numBytesSpilledEstimate));
        out["spilledRecords"] = opts.serializeLiteral(static_cast<long long>(stats.spilledRecords));
    }

    return out.freezeToValue();
}


void DocumentSourceGroupBase::doDispose() {
    _groupProcessor.reset();
}

boost::intrusive_ptr<DocumentSource> DocumentSourceGroupBase::optimize() {
    // Optimizing a 'DocumentSourceGroupBase' might modify its expressions to become incompatible
    // with SBE. We temporarily highjack the context's 'sbeCompatible' flag to communicate the
    // situation back to the 'DocumentSourceGroupBase'. Notice, that while a particular
    // 'DocumentSourceGroupBase' might become incompatible with SBE, other groups in the pipeline
    // and the collection access could be still eligible for lowering to SBE, thus we must reset the
    // context's 'sbeCompatible' flag back to its original value at the end of the 'optimize()'
    // call.
    //
    // TODO SERVER-XXXXX: replace this hack with a proper per-stage tracking of SBE compatibility.
    auto& idExpressions = _groupProcessor.getMutableIdExpressions();
    auto expCtx = idExpressions[0]->getExpressionContext();
    auto origSbeCompatibility = expCtx->sbeCompatibility;
    expCtx->sbeCompatibility = SbeCompatibility::noRequirements;

    // TODO: If all idExpressions are ExpressionConstants after optimization, then we know
    // there will be only one group. We should take advantage of that to avoid going through the
    // hash table.
    for (size_t i = 0; i < idExpressions.size(); i++) {
        idExpressions[i] = idExpressions[i]->optimize();
    }

    auto& accumulatedFields = _groupProcessor.getMutableAccumulationStatements();
    for (auto& accumulatedField : accumulatedFields) {
        accumulatedField.expr.initializer = accumulatedField.expr.initializer->optimize();
        accumulatedField.expr.argument = accumulatedField.expr.argument->optimize();
    }

    _sbeCompatibility = std::min(_sbeCompatibility, expCtx->sbeCompatibility);
    expCtx->sbeCompatibility = origSbeCompatibility;

    return this;
}

DepsTracker::State DocumentSourceGroupBase::getDependencies(DepsTracker* deps) const {
    // add the _id
    const auto& idExpressions = _groupProcessor.getIdExpressions();
    for (size_t i = 0; i < idExpressions.size(); i++) {
        expression::addDependencies(idExpressions[i].get(), deps);
    }

    // add the rest
    const auto& accumulatedFields = _groupProcessor.getAccumulationStatements();
    for (auto&& accumulatedField : accumulatedFields) {
        expression::addDependencies(accumulatedField.expr.argument.get(), deps);
        // Don't add initializer, because it doesn't refer to docs from the input stream.
    }

    return DepsTracker::State::EXHAUSTIVE_ALL;
}

void DocumentSourceGroupBase::addVariableRefs(std::set<Variables::Id>* refs) const {
    const auto& idExpressions = _groupProcessor.getIdExpressions();
    for (const auto& idExpr : idExpressions) {
        expression::addVariableRefs(idExpr.get(), refs);
    }

    const auto& accumulatedFields = _groupProcessor.getAccumulationStatements();
    for (auto&& accumulatedField : accumulatedFields) {
        expression::addVariableRefs(accumulatedField.expr.argument.get(), refs);
    }
}

DocumentSource::GetModPathsReturn DocumentSourceGroupBase::getModifiedPaths() const {
    // We preserve none of the fields, but any fields referenced as part of the group key are
    // logically just renamed.
    StringMap<std::string> renames;
    StringSet idFields;
    std::vector<std::string> listIdFields;
    const auto& idFieldNames = _groupProcessor.getIdFieldNames();
    const auto& idExpressions = _groupProcessor.getIdExpressions();
    for (std::size_t i = 0; i < idExpressions.size(); ++i) {
        auto idExp = idExpressions[i];
        auto pathToPutResultOfExpression = idFieldNames.empty() ? "_id" : "_id." + idFieldNames[i];
        auto computedPaths = idExp->getComputedPaths(pathToPutResultOfExpression);
        for (auto&& rename : computedPaths.renames) {
            renames[rename.first] = rename.second;
            idFields.insert(rename.second);
            listIdFields.push_back(rename.second);
        }
    }

    const auto& accumulatedFields = _groupProcessor.getAccumulationStatements();
    for (auto&& accumulatedField : accumulatedFields) {
        const auto& accumulationExpr = accumulatedField.expr;
        if (accumulationExpr.groupMatchOptimizationEligible) {
            const auto& expr = accumulationExpr.argument.get();
            auto& pathToPutResultOfExpression = accumulatedField.fieldName;
            auto fieldPath = dynamic_cast<ExpressionFieldPath*>(expr);
            if (fieldPath && fieldPath->isROOT()) {
                for (auto&& idField : listIdFields) {
                    // renames[to] = from
                    renames[pathToPutResultOfExpression + "." + idField] = idField;
                }
            } else {
                auto computedPaths = expr->getComputedPaths(pathToPutResultOfExpression);
                for (auto&& rename : computedPaths.renames) {
                    if (idFields.contains(rename.second)) {
                        // renames[to] = from
                        renames[rename.first] = rename.second;
                    }
                }
            }
        }
    }

    return {DocumentSource::GetModPathsReturn::Type::kAllExcept,
            OrderedPathSet{},  // No fields are preserved.
            std::move(renames)};
}

StringMap<boost::intrusive_ptr<Expression>> DocumentSourceGroupBase::getIdFields() const {
    const auto& idFieldNames = _groupProcessor.getIdFieldNames();
    const auto& idExpressions = _groupProcessor.getIdExpressions();
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
    return _groupProcessor.getMutableIdExpressions();
}

const std::vector<AccumulationStatement>& DocumentSourceGroupBase::getAccumulationStatements()
    const {
    return _groupProcessor.getAccumulationStatements();
}

std::vector<AccumulationStatement>& DocumentSourceGroupBase::getMutableAccumulationStatements() {
    return _groupProcessor.getMutableAccumulationStatements();
}

DocumentSourceGroupBase::DocumentSourceGroupBase(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<int64_t> maxMemoryUsageBytes)
    : DocumentSource(stageName, expCtx),
      _groupProcessor(expCtx,
                      maxMemoryUsageBytes ? *maxMemoryUsageBytes
                                          : internalDocumentSourceGroupMaxMemoryBytes.load()),
      _sbeCompatibility(SbeCompatibility::notCompatible) {}

namespace {

boost::intrusive_ptr<Expression> parseIdExpression(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement groupField,
    const VariablesParseState& vps) {
    if (groupField.type() == Object) {
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
                        !field.isNumber() && field.type() != Bool);
            }
            return ExpressionObject::parse(expCtx.get(), idKeyObj, vps);
        }
    } else {
        return Expression::parseOperand(expCtx.get(), groupField, vps);
    }
}

}  // namespace

boost::intrusive_ptr<Expression> DocumentSourceGroupBase::getIdExpression() const {
    return _groupProcessor.getIdExpression();
}

void DocumentSourceGroupBase::initializeFromBson(BSONElement elem) {
    uassert(15947, "a group's fields must be specified in an object", elem.type() == Object);

    const auto& idExpressions = _groupProcessor.getIdExpressions();
    BSONObj groupObj(elem.Obj());
    BSONObjIterator groupIterator(groupObj);
    VariablesParseState vps = pExpCtx->variablesParseState;
    pExpCtx->sbeGroupCompatibility = SbeCompatibility::noRequirements;
    while (groupIterator.more()) {
        BSONElement groupField(groupIterator.next());
        StringData pFieldName = groupField.fieldNameStringData();
        if (pFieldName == "_id") {
            uassert(15948, "a group's _id may only be specified once", idExpressions.empty());
            _groupProcessor.setIdExpression(parseIdExpression(pExpCtx, groupField, vps));
            invariant(!idExpressions.empty());
        } else if (pFieldName == "$doingMerge") {
            massert(17030, "$doingMerge should be true if present", groupField.Bool());

            _groupProcessor.setDoingMerge(true);
        } else if (isSpecFieldReserved(pFieldName)) {
            // No-op: field is used by the derived class.
        } else {
            // Any other field will be treated as an accumulator specification.
            _groupProcessor.addAccumulationStatement(
                AccumulationStatement::parseAccumulationStatement(pExpCtx.get(), groupField, vps));
        }
    }
    _sbeCompatibility = std::min(pExpCtx->sbeGroupCompatibility, pExpCtx->sbeCompatibility);

    uassert(15955, "a group specification must include an _id", !idExpressions.empty());
}

bool DocumentSourceGroupBase::pathIncludedInGroupKeys(const std::string& dottedPath) const {
    const auto& idExpressions = _groupProcessor.getIdExpressions();
    return std::any_of(idExpressions.begin(), idExpressions.end(), [&dottedPath](const auto& exp) {
        if (auto fieldExp = dynamic_cast<ExpressionFieldPath*>(exp.get())) {
            if (fieldExp->representsPath(dottedPath)) {
                return true;
            }
        }
        return false;
    });
}

bool DocumentSourceGroupBase::canRunInParallelBeforeWriteStage(
    const OrderedPathSet& nameOfShardKeyFieldsUponEntryToStage) const {
    if (_groupProcessor.doingMerge()) {
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

bool DocumentSourceGroupBase::isEligibleForTransformOnFirstDocument(
    GroupFromFirstDocumentTransformation::ExpectedInput& expectedInput,
    std::string& groupId) const {
    const auto& idExpressions = _groupProcessor.getIdExpressions();
    if (idExpressions.size() != 1) {
        // This transformation is only intended for $group stages that group on a single field.
        return false;
    }

    auto fieldPathExpr = dynamic_cast<ExpressionFieldPath*>(idExpressions.front().get());
    if (!fieldPathExpr || fieldPathExpr->isVariableReference()) {
        return false;
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
        return false;
    }

    groupId = fieldPath.tail().fullPath();

    // We do this transformation only if there are all $first, all $last, or no accumulators.
    const auto& accumulatedFields = _groupProcessor.getAccumulationStatements();
    if (accsNeedSameDoc(accumulatedFields, AccumulatorDocumentsNeeded::kFirstDocument)) {
        expectedInput = GroupFromFirstDocumentTransformation::ExpectedInput::kFirstDocument;
    } else if (accsNeedSameDoc(accumulatedFields, AccumulatorDocumentsNeeded::kLastDocument)) {
        expectedInput = GroupFromFirstDocumentTransformation::ExpectedInput::kLastDocument;
    } else {
        return false;
    }

    return true;
}

std::unique_ptr<GroupFromFirstDocumentTransformation>
DocumentSourceGroupBase::rewriteGroupAsTransformOnFirstDocument() const {
    std::string groupId;
    GroupFromFirstDocumentTransformation::ExpectedInput expectedInput;
    if (!isEligibleForTransformOnFirstDocument(expectedInput, groupId)) {
        return nullptr;
    }

    std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> fields;

    boost::intrusive_ptr<Expression> idField;
    const auto& idFieldNames = _groupProcessor.getIdFieldNames();
    // The _id field can be specified either as a fieldpath (ex. _id: "$a") or as a singleton
    // object (ex. _id: {v: "$a"}).
    if (idFieldNames.empty()) {
        idField = ExpressionFieldPath::deprecatedCreate(pExpCtx.get(), groupId);
    } else {
        invariant(idFieldNames.size() == 1);
        idField = ExpressionObject::create(
            pExpCtx.get(), {{idFieldNames.front(), _groupProcessor.getIdExpressions().front()}});
    }
    fields.emplace_back("_id", idField);

    for (auto&& accumulator : _groupProcessor.getAccumulationStatements()) {
        fields.emplace_back(accumulator.fieldName, accumulator.expr.argument);

        // Since we don't attempt this transformation for non-$first/$last accumulators,
        // the initializer should always be trivial.
    }

    return GroupFromFirstDocumentTransformation::create(
        pExpCtx, groupId, getSourceName(), std::move(fields), expectedInput);
}

boost::optional<DocumentSource::DistributedPlanLogic>
DocumentSourceGroupBase::distributedPlanLogic() {
    VariablesParseState vps = pExpCtx->variablesParseState;
    /* the merger will use the same grouping key */
    auto mergerGroupByExpression = ExpressionFieldPath::parse(pExpCtx.get(), "$$ROOT._id", vps);

    std::vector<AccumulationStatement> mergerAccumulators;
    const auto& accumulatedFields = _groupProcessor.getAccumulationStatements();
    mergerAccumulators.reserve(accumulatedFields.size());
    for (auto&& accumulatedField : accumulatedFields) {
        // The merger's output field names will be the same, as will the accumulator factories.
        // However, for some accumulators, the expression to be accumulated will be different. The
        // original accumulator may be collecting an expression based on a field expression or
        // constant.  Here, we accumulate the output of the same name from the prior group.
        auto copiedAccumulatedField = accumulatedField;
        copiedAccumulatedField.expr.argument = ExpressionFieldPath::parse(
            pExpCtx.get(), "$$ROOT." + copiedAccumulatedField.fieldName, vps);
        mergerAccumulators.emplace_back(std::move(copiedAccumulatedField));
    }

    // When merging, we always use generic hash based algorithm.
    boost::intrusive_ptr<DocumentSourceGroup> mergingGroup = DocumentSourceGroup::create(
        pExpCtx, std::move(mergerGroupByExpression), std::move(mergerAccumulators));
    mergingGroup->_groupProcessor.setDoingMerge(true);
    // {shardsStage, mergingStage, sortPattern}
    return DistributedPlanLogic{this, mergingGroup, boost::none};
}

}  // namespace mongo
