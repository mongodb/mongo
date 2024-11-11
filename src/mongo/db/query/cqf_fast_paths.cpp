/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/cqf_fast_paths.h"
#include "mongo/db/exec/inclusion_projection_executor.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/query/cqf_command_utils.h"
#include "mongo/db/query/cqf_fast_paths_utils.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/sbe_shared_helpers.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/sbe_stage_builder_projection.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::optimizer::fast_path {
namespace {

/**
 * Interface for implementing a fast path for a simple query of certain shape. Responsible for SBE
 * plan generation.
 */
class ExecTreeGenerator {
public:
    virtual ~ExecTreeGenerator() = default;

    virtual ExecTreeResult generateExecTree(const ExecTreeGeneratorParams& params) const = 0;

    virtual BSONObj generateExplain() const = 0;
};

/**
 * ABTPrinter implementation that passes on the given explain BSON. Used for implementing
 * explain for fast paths.
 */
class FastPathPrinter : public AbstractABTPrinter {
public:
    FastPathPrinter(BSONObj fastPathExplain) : _explainBSON(std::move(fastPathExplain)) {}

    BSONObj explainBSON() const final {
        return _explainBSON;
    }

    BSONObj explainQueryPlannerDebug() const final {
        return {};
    }

    std::string getPlanSummary() const final {
        return "COLLSCAN";
    }

    BSONObj getQueryParameters() const final {
        // Fast path queries don't get parameterized.
        return {};
    }

private:
    const BSONObj _explainBSON;
};

// We can use a BSON object representing the filter for fast and simple comparison.
using FastPathMap = FilterComparator::Map<std::unique_ptr<ExecTreeGenerator>>;

FastPathMap fastPathMap{FilterComparator::kInstance.makeLessThan()};

/**
 * Do not call this method directly. Instead, use the REGISTER_FAST_PATH macro defined in this
 * file.
 */
void registerExecTreeGenerator(BSONObj shape,
                               std::unique_ptr<ExecTreeGenerator> execTreeGenerator) {
    tassert(8321506,
            "Did not expect 'shape' to contain '_id' field or a dotted path",
            !containsSpecialField(shape));
    fastPathMap.insert({shape, std::move(execTreeGenerator)});
}

bool canUseFastPath(const bool hasIndexHint,
                    const MultipleCollectionAccessor& collections,
                    const CanonicalQuery* canonicalQuery,
                    const Pipeline* pipeline) {
    if (internalCascadesOptimizerDisableFastPath.load()) {
        return false;
    }
    if (internalQueryDefaultDOP.load() > 1) {
        // The current fast path implementations don't support parallel scan plans.
        return false;
    }
    if (hasIndexHint) {
        // The current fast path implementations only deal with collection scans.
        return false;
    }
    if (!collections.getMainCollection()) {
        // TODO SERVER-83267: Enable once we have a fast path for non-existent collections.
        return false;
    }
    if (!collections.getMainCollection()->getCollectionOptions().collation.isEmpty()) {
        // TODO SERVER-83716: The current fast path implementations don't support collation.
        return false;
    }
    if (canonicalQuery) {
        const auto& findRequest = canonicalQuery->getFindCommandRequest();
        if (findRequest.getLet() || canonicalQuery->getSortPattern() || findRequest.getLimit() ||
            findRequest.getSkip()) {
            // The current fast path implementations don't support sorting but do support empty
            // queries with a simple projection.
            return false;
        }
    }
    if (pipeline) {
        const auto& sources = pipeline->getSources();
        auto matchOrTransformationStage = [](const DocumentSource* stage) {
            return dynamic_cast<const DocumentSourceMatch*>(stage) ||
                dynamic_cast<const DocumentSourceSingleDocumentTransformation*>(stage);
        };
        // The current fast path implementations only support pipelines with max two stages.
        //
        // Currently supported pipeline patterns:
        //  1) Pipeline: []
        //  2) Pipeline: [{$match: <Simple predicate>}]
        //  3) Pipeline: [{$project: <Simple projection>}]
        //  4) Pipeline: [{$match: <Simple predicate>}, {$project: <Simple projection>}]
        if (sources.size() > 2)
            return false;
        if (sources.size() == 1 && !matchOrTransformationStage(sources.front().get()))
            return false;
        if (sources.size() == 2 &&
            (!dynamic_cast<const DocumentSourceMatch*>(sources.front().get()) ||
             !dynamic_cast<const DocumentSourceSingleDocumentTransformation*>(
                 sources.back().get())))
            return false;
    }
    const bool isSharded = collections.isAcquisition()
        ? collections.getMainAcquisition().getShardingDescription().isSharded()
        : collections.getMainCollection().isSharded_DEPRECATED();
    if (isSharded) {
        // The current fast path implementations don't support shard filtering.
        return false;
    }
    return true;
}

BSONObj extractQueryFilter(const CanonicalQuery* canonicalQuery, const Pipeline* pipeline) {
    if (canonicalQuery) {
        return canonicalQuery->getQueryObj();
    }
    if (pipeline) {
        return pipeline->getInitialQuery();
    }
    tasserted(8217100, "Expected canonicalQuery or pipeline.");
}

inline void extractFromStage(DocumentSource* stage,
                             const bool emptyFilter,
                             bool& projExists,
                             bool& projSupported,
                             std::vector<FieldRef>& projectedFields) {
    if (const auto transStage = dynamic_cast<DocumentSourceSingleDocumentTransformation*>(stage);
        transStage && transStage->getTransformationProcessor()) {
        projExists = true;
        if (!emptyFilter)
            return;
        const auto& transformer = transStage->getTransformer();
        const auto execPtr =
            dynamic_cast<const projection_executor::InclusionProjectionExecutor*>(&transformer);
        if (!execPtr) {
            projSupported = false;
            return;
        }

        if (execPtr->isInclusionOnly()) {
            const auto paths = execPtr->extractExhaustivePaths();
            if (paths) {
                std::copy(paths->begin(), paths->end(), std::back_inserter(projectedFields));
            }
        }

        // There may be unspported expression in the $project stage.
        if (projectedFields.size() == 0)
            projSupported = false;
    }
    return;
};

/*
 * Extract all the projected fields from the projection spec of a CanonicalQuery or of a Pipeline.
 *
 * If 'emptyFilter' is false, we only check if there is a projection without extracting any path.
 *
 * This function returns a vector of projected fields and a pair of boolean flags to determine which
 * fast path the query should use later on.
 *
 * 'projExists' indicates if there's a projection.
 *
 * 'projSupported' indicates if the projection is supported by 'EmptyQueryExecTreeGenerator'. This
 * flag is only useful if the filter is empty. The returned vector is useful if the projection is
 * supported.
 */
std::vector<FieldRef> extractProjectedFields(const CanonicalQuery* canonicalQuery,
                                             const Pipeline* pipeline,
                                             const bool emptyFilter,
                                             bool& projExists,
                                             bool& projSupported) {
    std::vector<FieldRef> projectedFields;
    if (canonicalQuery && !canonicalQuery->getFindCommandRequest().getProjection().isEmpty()) {
        const auto proj = canonicalQuery->getProj();
        projExists = true;
        if (!emptyFilter)
            return projectedFields;
        if (proj->isInclusionOnly()) {
            const auto& requiredFields = proj->getRequiredFields();
            for (const auto& field : requiredFields) {
                projectedFields.emplace_back(FieldRef{field});
            }
        } else {
            projSupported = false;
        }
    }

    if (pipeline) {
        const auto& stages = pipeline->getSources();
        tassert(
            805803, "The size of a Pipeline in CQF fast path cannot exceed 2", stages.size() <= 2);
        if (stages.size() == 1) {
            extractFromStage(
                stages.front().get(), emptyFilter, projExists, projSupported, projectedFields);
        } else if (stages.size() == 2) {
            extractFromStage(
                stages.front().get(), emptyFilter, projExists, projSupported, projectedFields);
            extractFromStage(
                stages.back().get(), emptyFilter, projExists, projSupported, projectedFields);
        }
    }

    bool hasId = std::find(projectedFields.begin(), projectedFields.end(), FieldRef{"_id"_sd}) !=
        projectedFields.end();
    if (projectedFields.size() >= 2 && !hasId)
        projSupported = false;

    return projectedFields;
}

// Work around https://gcc.gnu.org/bugzilla/show_bug.cgi?id=85282. These could be defined inside
// 'EExprBuilder' but GCC doesn't allow template specializations in non-namespace scopes.
namespace eexpr_helper {
using CaseValuePair =
    std::pair<std::unique_ptr<sbe::EExpression>, std::unique_ptr<sbe::EExpression>>;

template <typename... Ts>
std::unique_ptr<sbe::EExpression> buildMultiBranchConditional(Ts... cases);

template <typename... Ts>
std::unique_ptr<sbe::EExpression> buildMultiBranchConditional(CaseValuePair headCase, Ts... rest) {
    return sbe::makeE<sbe::EIf>(std::move(headCase.first),
                                std::move(headCase.second),
                                buildMultiBranchConditional(std::move(rest)...));
}

template <>
std::unique_ptr<sbe::EExpression> buildMultiBranchConditional(
    std::unique_ptr<sbe::EExpression> defaultCase) {
    return defaultCase;
}
}  // namespace eexpr_helper

/**
 * Exposes required SBE helper functions to 'sbe_helper::generateComparisonExpr'. Note that this
 * class is stateless and doesn't support collation.
 */
struct EExprBuilder {
    using CaseValuePair = eexpr_helper::CaseValuePair;

    template <typename... Args>
    static inline std::unique_ptr<sbe::EExpression> makeFunction(StringData name, Args&&... args) {
        return sbe::makeE<sbe::EFunction>(name, sbe::makeEs(std::forward<Args>(args)...));
    }

    static inline auto makeConstant(sbe::value::TypeTags tag, sbe::value::Value val) {
        return sbe::makeE<sbe::EConstant>(tag, val);
    }

    static inline auto makeNullConstant() {
        return makeConstant(sbe::value::TypeTags::Null, 0);
    }

    static inline auto makeBoolConstant(bool boolVal) {
        auto val = sbe::value::bitcastFrom<bool>(boolVal);
        return makeConstant(sbe::value::TypeTags::Boolean, val);
    }

    static inline auto makeInt32Constant(int32_t num) {
        auto val = sbe::value::bitcastFrom<int32_t>(num);
        return makeConstant(sbe::value::TypeTags::NumberInt32, val);
    }

    static inline auto makeMakeObjSpecConstant(std::unique_ptr<sbe::MakeObjSpec> objSpec) {
        auto val = sbe::value::bitcastFrom<sbe::MakeObjSpec*>(objSpec.release());
        return makeConstant(sbe::value::TypeTags::makeObjSpec, val);
    }

    static std::unique_ptr<sbe::EExpression> makeNot(std::unique_ptr<sbe::EExpression> e) {
        return sbe::makeE<sbe::EPrimUnary>(sbe::EPrimUnary::logicNot, std::move(e));
    }

    static std::unique_ptr<sbe::EExpression> makeVariable(sbe::value::SlotId slotId) {
        return sbe::makeE<sbe::EVariable>(slotId);
    }

    static std::unique_ptr<sbe::EExpression> makeBinaryOp(sbe::EPrimBinary::Op binaryOp,
                                                          std::unique_ptr<sbe::EExpression> lhs,
                                                          std::unique_ptr<sbe::EExpression> rhs) {
        return sbe::makeE<sbe::EPrimBinary>(binaryOp, std::move(lhs), std::move(rhs));
    }

    static constexpr auto makeBinaryOpWithCollation = makeBinaryOp;

    static std::unique_ptr<sbe::EExpression> generateNullOrMissing(
        std::unique_ptr<sbe::EExpression> expr) {
        return makeBinaryOp(sbe::EPrimBinary::fillEmpty,
                            makeFunction("typeMatch",
                                         expr->clone(),
                                         makeInt32Constant(getBSONTypeMask(BSONType::jstNULL))),
                            makeBoolConstant(true));
    }

    static std::unique_ptr<sbe::EExpression> makeFillEmptyFalse(
        std::unique_ptr<sbe::EExpression> e) {
        return makeBinaryOp(sbe::EPrimBinary::fillEmpty, std::move(e), makeBoolConstant(false));
    }

    static std::unique_ptr<sbe::EExpression> makeLocalLambda(
        sbe::FrameId frameId, std::unique_ptr<sbe::EExpression> expr) {
        return sbe::makeE<sbe::ELocalLambda>(frameId, std::move(expr));
    }

    template <typename... Args>
    static inline std::unique_ptr<sbe::EExpression> buildMultiBranchConditional(Args&&... args) {
        return eexpr_helper::buildMultiBranchConditional(std::forward<Args>(args)...);
    }

    static std::unique_ptr<sbe::EExpression> cloneExpr(
        const std::unique_ptr<sbe::EExpression>& expr) {
        return expr->clone();
    }
};

/**
 * Implements fast path SBE plan generation for a query and predicates - a simple collection scan.
 *
 * This generator also supports empty queries with a simple projection. A simple projection can be,
 * 1) only one top-level field inclusion projection w/ or w/o '_id' field.
 * 2) only one dotted field inclusion projection w/ or w/o '_id' field.
 * Inclusion projection cannot contain any expression.
 */
class EmptyQueryExecTreeGenerator : public ExecTreeGenerator {
public:
    ExecTreeResult generateExecTree(const ExecTreeGeneratorParams& params) const override {
        sbe::value::SlotIdGenerator ids;
        auto staticData = std::make_unique<stage_builder::PlanStageStaticData>();
        std::unique_ptr<sbe::PlanStage> sbePlan;

        // TODO SERVER-83628: respect the scanOrder
        auto makeScanStage = [&](boost::optional<sbe::value::SlotId> slotId,
                                 std::vector<std::string> fieldNames,
                                 sbe::value::SlotVector slotVector) {
            return sbe::makeS<sbe::ScanStage>(
                params.collectionUuid,
                params.dbName,
                slotId,
                boost::none /*scanRidSlot*/,
                boost::none /*recordIdSlot*/,
                boost::none /*snapshotIdSlot*/,
                boost::none /*indexIdentSlot*/,
                boost::none /*indexKeySlot*/,
                boost::none /*indexKeyPatternSlot*/,
                std::move(fieldNames),
                std::move(slotVector),
                boost::none /*seekRecordIdSlot*/,
                boost::none /*minRecordIdSlot*/,
                boost::none /*maxRecordIdSlot*/,
                true /*forwardScan*/,
                params.yieldPolicy,
                0 /*PlanNodeId*/,
                sbe::ScanCallbacks{{}, {}, {}},
                gDeprioritizeUnboundedUserCollectionScans.load() /*lowPriority*/
            );
        };

        // Hardcode the SBE plan for simple projections if exists. There should be only one non-_id
        // field to project.
        if (const auto& fields = params.projectFields; fields.size() > 0) {
            tassert(8058002, "Not eligible projection for fast path.", fields.size() <= 2);
            bool includeIdField =
                std::find(fields.begin(), fields.end(), FieldRef{"_id"_sd}) != fields.end();
            // If the field is a top-level field, we can use mkBsonObj stage rather than a project
            // stage for better performance.
            bool simpleProjection = true;
            std::vector<std::string> simpleFields;
            for (const auto& field : fields) {
                if (field.numParts() == 1) {
                    simpleFields.push_back(std::string(field.dottedField()));
                } else {
                    simpleProjection = false;
                }
            }

            if (simpleProjection) {
                // A simple projection means that the projected field is a top-level field which we
                // can push down to the scan stage.
                sbe::value::SlotVector slotVec;
                for (size_t i = 0; i < simpleFields.size(); i++) {
                    slotVec.push_back(ids.generate());
                }
                auto scanSlot = ids.generate();
                auto scanStage = makeScanStage(scanSlot, simpleFields, std::move(slotVec));

                staticData->resultSlot = ids.generate();
                sbePlan = sbe::makeS<sbe::MakeBsonObjStage>(
                    std::move(scanStage) /*A collScan stage*/,
                    *staticData->resultSlot /*objSlot*/,
                    scanSlot /*rootSlot*/,
                    sbe::MakeBsonObjStage::FieldBehavior::keep /*fieldBehavior*/,
                    simpleFields /*fields*/,
                    std::vector<std::string>{} /*projectFields*/,
                    sbe::makeSV() /*projectVars*/,
                    true /*forceNewObject*/,
                    false /*returnOldObject*/,
                    0 /*PlanNodeId*/);
            } else {
                // Given that we already know it's a non-simple projection and the size of 'fields'
                // cannot exceed 2, so it's safe to assume the dotted field is either the first or
                // the second field.
                auto dottedField = fields[0].numParts() > 1 ? fields[0] : fields[1];

                auto scanSlot = ids.generate();
                auto scanStage =
                    makeScanStage(scanSlot, std::vector<std::string>(), sbe::value::SlotVector());

                staticData->resultSlot = ids.generate();
                auto mkBsonFunc = EExprBuilder::makeFunction(
                    "makeBsonObj"_sd,
                    makeObjectSpecForDottedField(dottedField, includeIdField),
                    EExprBuilder::makeVariable(scanSlot),
                    EExprBuilder::makeBoolConstant(false));
                sbe::SlotExprPairVector projExprs;
                projExprs.emplace_back(
                    std::make_pair(*staticData->resultSlot, std::move(mkBsonFunc)));

                sbePlan =
                    sbe::makeS<sbe::ProjectStage>(std::move(scanStage), std::move(projExprs), 0);
            }
        } else {
            // A COLLSCAN plan if there's no projection.
            staticData->resultSlot = ids.generate();
            sbePlan = makeScanStage(
                staticData->resultSlot, std::vector<std::string>(), sbe::value::SlotVector());
        }

        stage_builder::PlanStageData data{
            stage_builder::Environment{std::make_unique<sbe::RuntimeEnvironment>()},
            std::move(staticData)};

        return {std::move(sbePlan), std::move(data)};
    }

    BSONObj generateExplain() const override {
        return BSON("stage"
                    << "FASTPATH"
                    << "type"
                    << "emptyFind");
    }

private:
    // This helper makes a "makeBsonObj" for the projection specification in the sbe::project stage.
    // E.g. {"a.b.c":1, "_id": 1} should generate: makeBsonObj(MakeObjSpec([x = MakeObj([y =
    // MakeObj([z = MakeObj([a], Closed, RetNothing)], Closed, RetNothing)], Closed, RetNothing),
    // _id], Closed, RetNothing)
    std::unique_ptr<sbe::EExpression> makeObjectSpecForDottedField(const FieldRef& field,
                                                                   bool includeIdField) const {
        std::unique_ptr<sbe::MakeObjSpec> projObj = nullptr;
        for (auto idx = field.numParts() - 1; idx >= 0; idx--) {
            std::vector<std::string> fields{std::string(field.getPart(idx))};
            std::vector<sbe::MakeObjSpec::FieldAction> actions;
            if (projObj) {
                actions.emplace_back(std::move(projObj));
            } else {
                actions.emplace_back(sbe::MakeObjSpec::Keep{});
            }
            // If "_id" field is included, add "_id" field in the top-level object.
            if (idx == 0 && includeIdField) {
                fields.emplace_back("_id"_sd);
                actions.emplace_back(sbe::MakeObjSpec::Keep{});
            }
            projObj = std::make_unique<sbe::MakeObjSpec>(
                FieldListScope::kClosed,
                std::move(fields),
                std::move(actions),
                sbe::MakeObjSpec::NonObjInputBehavior::kReturnNothing);
        }

        return EExprBuilder::makeMakeObjSpecConstant(std::move(projObj));
    }
};

REGISTER_FAST_PATH_EXEC_TREE_GENERATOR(Empty, {}, std::make_unique<EmptyQueryExecTreeGenerator>());

/**
 * Implements fast path SBE plan generation for a query with a single comparison predicate on a
 * top-level field.
 */
class SingleFieldQueryExecTreeGenerator final : public ExecTreeGenerator {
public:
    SingleFieldQueryExecTreeGenerator(Operations op) : _op(op) {}

    ExecTreeResult generateExecTree(const ExecTreeGeneratorParams& params) const override {
        const auto props = makeSinglePredicateCollScanProps(params.filter);

        sbe::value::SlotIdGenerator ids;
        auto staticData = std::make_unique<stage_builder::PlanStageStaticData>(
            stage_builder::PlanStageStaticData{.resultSlot = ids.generate()});

        const auto fieldSlotId = ids.generate();
        sbe::value::SlotVector scanFieldSlots{fieldSlotId};
        std::vector<std::string> scanFieldNames{props.fieldName.value().toString()};

        const PlanNodeId planNodeId{0};

        auto scanStage = sbe::makeS<sbe::ScanStage>(
            params.collectionUuid,
            params.dbName,
            staticData->resultSlot,
            boost::none /*scanRidSlot*/,
            boost::none,
            boost::none,
            boost::none,
            boost::none,
            boost::none,
            scanFieldNames,
            scanFieldSlots,
            boost::none /*seekRecordIdSlot*/,
            boost::none /*minRecordIdSlot*/,
            boost::none /*maxRecordIdSlot*/,
            true /*forwardScan*/,
            params.yieldPolicy,
            planNodeId,
            sbe::ScanCallbacks{{}, {}, {}},
            gDeprioritizeUnboundedUserCollectionScans.load() /*lowPriority*/);

        const sbe::FrameId cmpFrameId{0};
        const sbe::value::SlotId varId{0};

        auto comparisonExpr = generateComparisonExpr(
            props.constant, sbe::makeE<sbe::EVariable>(cmpFrameId, varId, true /*move*/));

        auto lambdaExpr = EExprBuilder::makeLocalLambda(cmpFrameId, std::move(comparisonExpr));

        auto traverseExpr = EExprBuilder::makeFunction(
            "traverseF",
            sbe::makeE<sbe::EVariable>(fieldSlotId),
            std::move(lambdaExpr),
            EExprBuilder::makeBoolConstant(shouldCompareArray(props.constant)));

        auto sbePlan = sbe::makeS<sbe::FilterStage<false>>(
            std::move(scanStage), std::move(traverseExpr), planNodeId);

        stage_builder::PlanStageData data{
            stage_builder::Environment{std::make_unique<sbe::RuntimeEnvironment>()},
            std::move(staticData)};

        return {std::move(sbePlan), std::move(data)};
    }

    BSONObj generateExplain() const override {
        return BSON("stage"
                    << "FASTPATH"
                    << "type"
                    << "singlePredicateCollScan");
    }

private:
    const Operations _op;

    /**
     * Holds properties of a single MQL predicate. These are extracted from a BSON
     * representing the query filter.
     */
    struct SinglePredicateCollScanProps {
        optimizer::FieldNameType fieldName;
        // We don't necessarily have to depend on ABT here, but 'Constant' is convenient
        // for holding and appropriately deleting the SBE value.
        optimizer::Constant constant;
    };

    bool shouldCompareArray(const optimizer::Constant& constant) const {
        // When the constant is an array, MinKey, or MaxKey, we need to enable comparisons to
        // arrays too.
        const auto [constTag, constVal] = constant.get();
        return constTag == sbe::value::TypeTags::bsonArray ||
            constTag == sbe::value::TypeTags::MinKey || constTag == sbe::value::TypeTags::MaxKey;
    }

    SinglePredicateCollScanProps makeSinglePredicateCollScanProps(const BSONObj& filter) const {
        const auto& elem = filter.firstElement();

        // Note that the constructor of 'FieldNameType' copies the contents of the string view, so
        // this is safe even if the filter doesn't outlive the props.
        const optimizer::FieldNameType fieldName{elem.fieldNameStringData()};

        const auto makeConstant = [](const BSONElement& elem) {
            auto [tag, val] = sbe::bson::convertFrom<true /*View*/>(
                elem.rawdata(), elem.rawdata() + elem.size(), elem.fieldNameSize() - 1);

            ABT constant = optimizer::Constant::createFromCopy(tag, val);
            return *constant.cast<optimizer::Constant>();
        };
        const auto constant = [&] {
            // We assume the predicate is either:
            // - '{field: value}'
            // - '{field: {$op: value}}'
            if (elem.isABSONObj()) {
                for (auto&& child : elem.Obj()) {
                    tassert(8217102,
                            "Expected predicate on top-level field.",
                            child.fieldName()[0] == '$');
                    return makeConstant(child);
                }
            }
            return makeConstant(elem);
        }();

        return {fieldName, std::move(constant)};
    }

    std::unique_ptr<sbe::EExpression> generateComparisonExpr(
        const optimizer::Constant& constant, std::unique_ptr<sbe::EExpression> inputExpr) const {
        EExprBuilder builder{};

        const auto sbeOp = getEPrimBinaryOp(_op);
        const auto [tag, val] = constant.get();

        sbe_helper::ValueExpressionFn<std::unique_ptr<sbe::EExpression>> makeValExpr =
            [](sbe::value::TypeTags tag, sbe::value::Value val) {
                auto [copyTag, copyVal] = sbe::value::copyValue(tag, val);
                return sbe::makeE<sbe::EConstant>(copyTag, copyVal);
            };

        return sbe_helper::generateComparisonExpr(
            builder, tag, val, sbeOp, std::move(inputExpr), std::move(makeValExpr));
    }
};

// Matches, for example, '{a: 1}'.
REGISTER_FAST_PATH_EXEC_TREE_GENERATOR(
    Eq1, BSON("ignore" << 0), std::make_unique<SingleFieldQueryExecTreeGenerator>(Operations::Eq));
// Matches, for example, '{a: {$eq: 1}}'.
REGISTER_FAST_PATH_EXEC_TREE_GENERATOR(
    Eq2,
    BSON("ignore" << BSON("$eq" << 0)),
    std::make_unique<SingleFieldQueryExecTreeGenerator>(Operations::Eq));
// Matches, for example, '{a: {$lt: 1}}'.
REGISTER_FAST_PATH_EXEC_TREE_GENERATOR(
    Lt,
    BSON("ignore" << BSON("$lt" << 0)),
    std::make_unique<SingleFieldQueryExecTreeGenerator>(Operations::Lt));
// Matches, for example, '{a: {$lte: 1}}'.
REGISTER_FAST_PATH_EXEC_TREE_GENERATOR(
    Lte,
    BSON("ignore" << BSON("$lte" << 0)),
    std::make_unique<SingleFieldQueryExecTreeGenerator>(Operations::Lte));
// Matches, for example, '{a: {$gt: 1}}'.
REGISTER_FAST_PATH_EXEC_TREE_GENERATOR(
    Gt,
    BSON("ignore" << BSON("$gt" << 0)),
    std::make_unique<SingleFieldQueryExecTreeGenerator>(Operations::Gt));
// Matches, for example, '{a: {$gte: 1}}'.
REGISTER_FAST_PATH_EXEC_TREE_GENERATOR(
    Gte,
    BSON("ignore" << BSON("$gte" << 0)),
    std::make_unique<SingleFieldQueryExecTreeGenerator>(Operations::Gte));
}  // namespace

const ExecTreeGenerator* getFastPathExecTreeGenerator(const ExecTreeGeneratorParams& params) {
    auto generatorIt = fastPathMap.find(params.filter);
    if (generatorIt == fastPathMap.end()) {
        OPTIMIZER_DEBUG_LOG(8321501,
                            5,
                            "Query not eligible for a fast path.",
                            "query"_attr = params.filter.toString());
        return {};
    }

    const auto& fields = params.projectFields;
    // Check if 'projectedFields' is eligible for EmptyQueryExecTreeGenerator.
    if (dynamic_cast<EmptyQueryExecTreeGenerator*>(generatorIt->second.get())) {
        // Bail out if there is a projection but not yet supported.
        if (!params.projSupported || fields.size() > 2)
            return nullptr;
    } else if (params.projExists) {
        // Projection is only supported in empty queries.
        return nullptr;
    }

    OPTIMIZER_DEBUG_LOG(
        8321502, 5, "Using a fast path for query", "query"_attr = params.filter.toString());

    return generatorIt->second.get();
}

ExecTreeResult getFastPathExecTreeForTest(const ExecTreeGeneratorParams& params) {
    auto generator = getFastPathExecTreeGenerator(params);
    tassert(8217103, "Filter is not eligible for a fast path.", generator);

    return generator->generateExecTree(params);
}

boost::optional<ExecParams> tryGetSBEExecutorViaFastPath(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    const MultipleCollectionAccessor& collections,
    const bool hasExplain,
    const boost::optional<BSONObj> indexHint,
    const Pipeline* pipeline,
    const CanonicalQuery* canonicalQuery) {
    validateCommandOptions(canonicalQuery, collections.getMainCollection(), indexHint, {});

    const bool hasIndexHint = indexHint && !indexHint->isEmpty();
    if (!canUseFastPath(hasIndexHint, collections, canonicalQuery, pipeline)) {
        return {};
    }

    const auto filter = extractQueryFilter(canonicalQuery, pipeline);

    bool projExists = false;
    bool projSupported = true;
    std::vector<FieldRef> projection = extractProjectedFields(
        canonicalQuery, pipeline, filter.isEmpty(), projExists, projSupported);

    std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy =
        PlanYieldPolicySBE::make(opCtx, PlanYieldPolicy::YieldPolicy::YIELD_AUTO, collections, nss);

    ExecTreeGeneratorParams params{collections.getMainCollection()->uuid(),
                                   collections.getMainCollection()->ns().dbName(),
                                   sbeYieldPolicy.get(),
                                   filter,
                                   projection,
                                   projExists,
                                   projSupported};

    auto generator = getFastPathExecTreeGenerator(params);
    if (!generator) {
        return {};
    }

    auto [sbePlan, data] = generator->generateExecTree(params);

    {
        sbe::DebugPrinter p;
        OPTIMIZER_DEBUG_LOG(6264802, 5, "Lowered SBE plan", "plan"_attr = p.print(*sbePlan.get()));
    }

    sbePlan->attachToOperationContext(opCtx);
    if (expCtx->explain || expCtx->mayDbProfile) {
        sbePlan->markShouldCollectTimingInfo();
    }

    auto explain = hasExplain ? generator->generateExplain() : BSONObj{};

    sbePlan->prepare(data.env.ctx);
    CurOp::get(opCtx)->stopQueryPlanningTimer();

    // Note that since we are not doing any optimization in the fast path (and thus will have
    // nothing to record for explain purposes), we create an empty OptimizerCounterInfo object
    // below.
    return {{opCtx,
             nullptr /*solution*/,
             {std::move(sbePlan), std::move(data)},
             std::make_unique<FastPathPrinter>(std::move(explain)),
             QueryPlannerParams::Options::DEFAULT,
             nss,
             std::move(sbeYieldPolicy),
             false /*isFromPlanCache*/,
             true /* generatedByBonsai */,
             nullptr /*pipelineMatchExpr*/,
             {} /* optCounterInfo */}};
}

boost::optional<ExecParams> tryGetSBEExecutorViaFastPath(
    const MultipleCollectionAccessor& collections, const CanonicalQuery* query) {
    boost::optional<BSONObj> indexHint;
    if (!query->getFindCommandRequest().getHint().isEmpty()) {
        indexHint = query->getFindCommandRequest().getHint();
    }

    return tryGetSBEExecutorViaFastPath(query->getOpCtx(),
                                        query->getExpCtx(),
                                        query->nss(),
                                        collections,
                                        query->getExplain().has_value(),
                                        indexHint,
                                        nullptr /*pipeline*/,
                                        query);
}

}  // namespace mongo::optimizer::fast_path
