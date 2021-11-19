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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder.h"

#include <fmt/format.h>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/hash_join.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/merge_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/sorted_merge.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/values/sort_spec.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/fts/fts_index_format.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/query/sbe_stage_builder_accumulator.h"
#include "mongo/db/query/sbe_stage_builder_coll_scan.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_filter.h"
#include "mongo/db/query/sbe_stage_builder_index_scan.h"
#include "mongo/db/query/sbe_stage_builder_projection.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/logv2/log.h"

namespace mongo::stage_builder {
namespace {
/**
 * Tree representing index key pattern or a subset of it.
 *
 * For example, the key pattern {a.b: 1, x: 1, a.c: 1} would look like:
 *
 *         <root>
 *         /   |
 *        a    x
 *       / \
 *      b   c
 *
 * This tree is used for building SBE subtrees to re-hydrate index keys and for covered projections.
 */
struct IndexKeyPatternTreeNode {
    IndexKeyPatternTreeNode* emplace(StringData fieldComponent) {
        auto newNode = std::make_unique<IndexKeyPatternTreeNode>();
        const auto newNodeRaw = newNode.get();
        children.emplace(fieldComponent, std::move(newNode));
        childrenOrder.push_back(fieldComponent.toString());

        return newNodeRaw;
    }

    /**
     * Returns leaf node matching field path. If the field path provided resolves to a non-leaf
     * node, null will be returned.
     *
     * For example, if tree was built for key pattern {a: 1, a.b: 1}, this method will return
     * nullptr for field path "a". On the other hand, this method will return corresponding node for
     * field path "a.b".
     */
    IndexKeyPatternTreeNode* findLeafNode(const FieldRef& fieldRef, size_t currentIndex = 0) {
        if (currentIndex == fieldRef.numParts()) {
            if (children.empty()) {
                return this;
            }
            return nullptr;
        }

        auto currentPart = fieldRef.getPart(currentIndex);
        if (auto it = children.find(currentPart); it != children.end()) {
            return it->second->findLeafNode(fieldRef, currentIndex + 1);
        } else {
            return nullptr;
        }
    }

    StringMap<std::unique_ptr<IndexKeyPatternTreeNode>> children;
    std::vector<std::string> childrenOrder;

    // Which slot the index key for this component is stored in. May be boost::none for non-leaf
    // nodes.
    boost::optional<sbe::value::SlotId> indexKeySlot;
};

/**
 * For covered projections, each of the projection field paths represent respective index key. To
 * rehydrate index keys into the result object, we first need to convert projection AST into
 * 'IndexKeyPatternTreeNode' structure. Context structure and visitors below are used for this
 * purpose.
 */
struct IndexKeysBuilderContext {
    // Contains resulting tree of index keys converted from projection AST.
    IndexKeyPatternTreeNode root;

    // Full field path of the currently visited projection node.
    std::vector<StringData> currentFieldPath;

    // Each projection node has a vector of field names. This stack contains indexes of the
    // currently visited field names for each of the projection nodes.
    std::vector<size_t> currentFieldIndex;
};

/**
 * Covered projections are always inclusion-only, so we ban all other operators.
 */
class IndexKeysBuilder : public projection_ast::ProjectionASTConstVisitor {
public:
    using projection_ast::ProjectionASTConstVisitor::visit;

    IndexKeysBuilder(IndexKeysBuilderContext* context) : _context{context} {}

    void visit(const projection_ast::ProjectionPositionalASTNode* node) final {
        tasserted(5474501, "Positional projection is not allowed in covered projection");
    }

    void visit(const projection_ast::ProjectionSliceASTNode* node) final {
        tasserted(5474502, "$slice is not allowed in covered projection");
    }

    void visit(const projection_ast::ProjectionElemMatchASTNode* node) final {
        tasserted(5474503, "$elemMatch is not allowed in covered projection");
    }

    void visit(const projection_ast::ExpressionASTNode* node) final {
        tasserted(5474504, "Expressions are not allowed in covered projection");
    }

    void visit(const projection_ast::MatchExpressionASTNode* node) final {
        tasserted(5474505,
                  "$elemMatch and positional projection are not allowed in covered projection");
    }

    void visit(const projection_ast::BooleanConstantASTNode* node) override {}

protected:
    IndexKeysBuilderContext* _context;
};

class IndexKeysPreBuilder final : public IndexKeysBuilder {
public:
    using IndexKeysBuilder::IndexKeysBuilder;
    using IndexKeysBuilder::visit;

    void visit(const projection_ast::ProjectionPathASTNode* node) final {
        _context->currentFieldIndex.push_back(0);
        _context->currentFieldPath.emplace_back(node->fieldNames().front());
    }
};

class IndexKeysInBuilder final : public IndexKeysBuilder {
public:
    using IndexKeysBuilder::IndexKeysBuilder;
    using IndexKeysBuilder::visit;

    void visit(const projection_ast::ProjectionPathASTNode* node) final {
        auto& currentIndex = _context->currentFieldIndex.back();
        currentIndex++;
        _context->currentFieldPath.back() = node->fieldNames()[currentIndex];
    }
};

class IndexKeysPostBuilder final : public IndexKeysBuilder {
public:
    using IndexKeysBuilder::IndexKeysBuilder;
    using IndexKeysBuilder::visit;

    void visit(const projection_ast::ProjectionPathASTNode* node) final {
        _context->currentFieldIndex.pop_back();
        _context->currentFieldPath.pop_back();
    }

    void visit(const projection_ast::BooleanConstantASTNode* constantNode) final {
        if (!constantNode->value()) {
            // Even though only inclusion is allowed in covered projection, there still can be
            // {_id: 0} component. We do not need to generate any nodes for it.
            return;
        }

        // Insert current field path into the index keys tree if it does not exist yet.
        auto* node = &_context->root;
        for (const auto& part : _context->currentFieldPath) {
            if (auto it = node->children.find(part); it != node->children.end()) {
                node = it->second.get();
            } else {
                node = node->emplace(part);
            }
        }
    }
};

/**
 * Given a key pattern and an array of slots of equal size, builds an IndexKeyPatternTreeNode
 * representing the mapping between key pattern component and slot.
 *
 * Note that this will "short circuit" in cases where the index key pattern contains two components
 * where one is a subpath of the other. For example with the key pattern {a:1, a.b: 1}, the "a.b"
 * component will not be represented in the output tree. For the purpose of rehydrating index keys,
 * this is fine (and actually preferable).
 */
std::unique_ptr<IndexKeyPatternTreeNode> buildKeyPatternTree(const BSONObj& keyPattern,
                                                             const sbe::value::SlotVector& slots) {
    size_t i = 0;

    auto root = std::make_unique<IndexKeyPatternTreeNode>();
    for (auto&& elem : keyPattern) {
        auto* node = root.get();
        bool skipElem = false;

        FieldRef fr(elem.fieldNameStringData());
        for (FieldIndex j = 0; j < fr.numParts(); ++j) {
            const auto part = fr.getPart(j);
            if (auto it = node->children.find(part); it != node->children.end()) {
                node = it->second.get();
                if (node->indexKeySlot) {
                    // We're processing the a sub-path of a path that's already indexed.  We can
                    // bail out here since we won't use the sub-path when reconstructing the
                    // object.
                    skipElem = true;
                    break;
                }
            } else {
                node = node->emplace(part);
            }
        }

        if (!skipElem) {
            node->indexKeySlot = slots[i];
        }

        ++i;
    }

    return root;
}

/**
 * Given a root IndexKeyPatternTreeNode, this function will construct an SBE expression for
 * producing a partial object from an index key.
 *
 * For example, given the index key pattern {a.b: 1, x: 1, a.c: 1} and the index key
 * {"": 1, "": 2, "": 3}, the SBE expression would produce the object {a: {b:1, c: 3}, x: 2}.
 */
std::unique_ptr<sbe::EExpression> buildNewObjExpr(const IndexKeyPatternTreeNode* kpTree) {

    sbe::EExpression::Vector args;
    for (auto&& fieldName : kpTree->childrenOrder) {
        auto it = kpTree->children.find(fieldName);

        args.emplace_back(makeConstant(fieldName));
        if (it->second->indexKeySlot) {
            args.emplace_back(makeVariable(*it->second->indexKeySlot));
        } else {
            // The reason this is in an else branch is that in the case where we have an index key
            // like {a.b: ..., a: ...}, we've already made the logic for reconstructing the 'a'
            // portion, so the 'a.b' subtree can be skipped.
            args.push_back(buildNewObjExpr(it->second.get()));
        }
    }

    return sbe::makeE<sbe::EFunction>("newObj", std::move(args));
}

/**
 * Given a stage, and index key pattern a corresponding array of slot IDs, this function
 * add a ProjectStage to the tree which rehydrates the index key and stores the result in
 * 'resultSlot.'
 */
std::unique_ptr<sbe::PlanStage> rehydrateIndexKey(std::unique_ptr<sbe::PlanStage> stage,
                                                  const BSONObj& indexKeyPattern,
                                                  PlanNodeId nodeId,
                                                  const sbe::value::SlotVector& indexKeySlots,
                                                  sbe::value::SlotId resultSlot) {
    auto kpTree = buildKeyPatternTree(indexKeyPattern, indexKeySlots);
    auto keyExpr = buildNewObjExpr(kpTree.get());

    return sbe::makeProjectStage(std::move(stage), nodeId, resultSlot, std::move(keyExpr));
}

/**
 * Generates an EOF plan. Note that even though this plan will return nothing, it will still define
 * the slots specified by 'reqs'.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateEofPlan(
    PlanNodeId nodeId, const PlanStageReqs& reqs, sbe::value::SlotIdGenerator* slotIdGenerator) {
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projects;

    PlanStageSlots outputs(reqs, slotIdGenerator);
    outputs.forEachSlot(reqs, [&](auto&& slot) {
        projects.insert({slot, sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0)});
    });

    auto stage = sbe::makeS<sbe::LimitSkipStage>(
        sbe::makeS<sbe::CoScanStage>(nodeId), 0, boost::none, nodeId);

    if (!projects.empty()) {
        // Even though this SBE tree will produce zero documents, we still need a ProjectStage to
        // define the slots in 'outputSlots' so that calls to getAccessor() won't fail.
        stage = sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projects), nodeId);
    }

    return {std::move(stage), std::move(outputs)};
}
}  // namespace

std::unique_ptr<sbe::RuntimeEnvironment> makeRuntimeEnvironment(
    const CanonicalQuery& cq,
    OperationContext* opCtx,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    auto env = std::make_unique<sbe::RuntimeEnvironment>();

    // Register an unowned global timezone database for datetime expression evaluation.
    env->registerSlot("timeZoneDB"_sd,
                      sbe::value::TypeTags::timeZoneDB,
                      sbe::value::bitcastFrom<const TimeZoneDatabase*>(getTimeZoneDatabase(opCtx)),
                      false,
                      slotIdGenerator);

    if (auto collator = cq.getCollator(); collator) {
        env->registerSlot("collator"_sd,
                          sbe::value::TypeTags::collator,
                          sbe::value::bitcastFrom<const CollatorInterface*>(collator),
                          false,
                          slotIdGenerator);
    }

    for (auto&& [id, name] : Variables::kIdToBuiltinVarName) {
        if ((id != Variables::kRootId && id != Variables::kRemoveId &&
             cq.getExpCtx()->variables.hasValue(id))) {
            auto [tag, val] = makeValue(cq.getExpCtx()->variables.getValue(id));
            env->registerSlot(name, tag, val, true, slotIdGenerator);
        } else if (id == Variables::kSearchMetaId) {
            // SEARCH_META never has a value at this point but can be set later and therefore must
            // have a slot. The find layer is not responsible for setting this value.
            auto [tag, val] = makeValue(cq.getExpCtx()->variables.getValue(id));
            env->registerSlot(name, tag, val, true, slotIdGenerator);
        }
    }

    return env;
}

PlanStageSlots::PlanStageSlots(const PlanStageReqs& reqs,
                               sbe::value::SlotIdGenerator* slotIdGenerator) {
    for (auto&& [slotName, isRequired] : reqs._slots) {
        if (isRequired) {
            _slots[slotName] = slotIdGenerator->generate();
        }
    }
}

std::string PlanStageData::debugString() const {
    StringBuilder builder;

    if (auto slot = outputs.getIfExists(PlanStageSlots::kResult); slot) {
        builder << "$$RESULT=s" << *slot << " ";
    }
    if (auto slot = outputs.getIfExists(PlanStageSlots::kRecordId); slot) {
        builder << "$$RID=s" << *slot << " ";
    }

    env->debugString(&builder);

    return builder.str();
}

namespace {
void getAllNodesByTypeHelper(const QuerySolutionNode* root,
                             StageType type,
                             std::vector<const QuerySolutionNode*>& results) {
    if (root->getType() == type) {
        results.push_back(root);
    }

    for (auto&& child : root->children) {
        getAllNodesByTypeHelper(child, type, results);
    }
}

std::vector<const QuerySolutionNode*> getAllNodesByType(const QuerySolutionNode* root,
                                                        StageType type) {
    std::vector<const QuerySolutionNode*> results;
    getAllNodesByTypeHelper(root, type, results);
    return results;
}

/**
 * Returns pair consisting of:
 *  - First node of the specified type found by pre-order traversal. If node was not found, this
 *    pair element is nullptr.
 *  - Total number of nodes with the specified type in tree.
 */
std::pair<const QuerySolutionNode*, size_t> getFirstNodeByType(const QuerySolutionNode* root,
                                                               StageType type) {
    const QuerySolutionNode* result = nullptr;
    size_t count = 0;
    if (root->getType() == type) {
        result = root;
        count++;
    }

    for (auto&& child : root->children) {
        auto [subTreeResult, subTreeCount] = getFirstNodeByType(child, type);
        if (!result) {
            result = subTreeResult;
        }
        count += subTreeCount;
    }

    return {result, count};
}

/**
 * Returns node of the specified type found in tree. If there is no such node, returns null. If
 * there are more than one nodes of the specified type, throws an exception.
 */
const QuerySolutionNode* getLoneNodeByType(const QuerySolutionNode* root, StageType type) {
    auto [result, count] = getFirstNodeByType(root, type);
    const auto msgCount = count;
    tassert(5474506,
            str::stream() << "Found " << msgCount << " nodes of type " << stageTypeToString(type)
                          << ", expected one or zero",
            count < 2);
    return result;
}

/**
 * Callback function that logs a message and uasserts if it detects a corrupt index key. An index
 * key is considered corrupt if it has no corresponding Record.
 */
void indexKeyCorruptionCheckCallback(OperationContext* opCtx,
                                     sbe::value::SlotAccessor* snapshotIdAccessor,
                                     sbe::value::SlotAccessor* indexKeyAccessor,
                                     sbe::value::SlotAccessor* indexKeyPatternAccessor,
                                     const RecordId& rid,
                                     const NamespaceString& nss) {
    // Having a recordId but no record is only an issue when we are not ignoring prepare conflicts.
    if (opCtx->recoveryUnit()->getPrepareConflictBehavior() == PrepareConflictBehavior::kEnforce) {
        tassert(5113700, "Should have snapshot id accessor", snapshotIdAccessor);
        auto currentSnapshotId = opCtx->recoveryUnit()->getSnapshotId();
        auto [snapshotIdTag, snapshotIdVal] = snapshotIdAccessor->getViewOfValue();
        const auto msgSnapshotIdTag = snapshotIdTag;
        tassert(5113701,
                str::stream() << "SnapshotId is of wrong type: " << msgSnapshotIdTag,
                snapshotIdTag == sbe::value::TypeTags::NumberInt64);
        auto snapshotId = sbe::value::bitcastTo<uint64_t>(snapshotIdVal);

        // If we have a recordId but no corresponding record, this means that said record has been
        // deleted. This can occur during yield, in which case the snapshot id would be incremented.
        // If, on the other hand, the current snapshot id matches that of the recordId, this
        // indicates an error as no yield could have taken place.
        if (snapshotId == currentSnapshotId.toNumber()) {
            tassert(5113703, "Should have index key accessor", indexKeyAccessor);
            tassert(5113704, "Should have key pattern accessor", indexKeyPatternAccessor);

            auto [ksTag, ksVal] = indexKeyAccessor->getViewOfValue();
            auto [kpTag, kpVal] = indexKeyPatternAccessor->getViewOfValue();

            const auto msgKsTag = ksTag;
            tassert(5113706,
                    str::stream() << "KeyString is of wrong type: " << msgKsTag,
                    ksTag == sbe::value::TypeTags::ksValue);

            const auto msgKpTag = kpTag;
            tassert(5113707,
                    str::stream() << "Index key pattern is of wrong type: " << msgKpTag,
                    kpTag == sbe::value::TypeTags::bsonObject);

            auto keyString = sbe::value::getKeyStringView(ksVal);
            tassert(5113708, "KeyString does not exist", keyString);

            BSONObj bsonKeyPattern(sbe::value::bitcastTo<const char*>(kpVal));
            auto bsonKeyString = KeyString::toBson(*keyString, Ordering::make(bsonKeyPattern));
            auto hydratedKey = IndexKeyEntry::rehydrateKey(bsonKeyPattern, bsonKeyString);

            LOGV2_ERROR_OPTIONS(
                5113709,
                {logv2::UserAssertAfterLog(ErrorCodes::DataCorruptionDetected)},
                "Erroneous index key found with reference to non-existent record id. Consider "
                "dropping and then re-creating the index and then running the validate command "
                "on the collection.",
                "namespace"_attr = nss,
                "recordId"_attr = rid,
                "indexKeyData"_attr = hydratedKey);
        }
    }
}

/**
 * Callback function that returns true if a given index key is valid, false otherwise. An index key
 * is valid if either the snapshot id of the underlying index scan matches the current snapshot id,
 * or that the index keys are still part of the underlying index.
 */
bool indexKeyConsistencyCheckCallback(OperationContext* opCtx,
                                      StringMap<const IndexAccessMethod*> iamTable,
                                      sbe::value::SlotAccessor* snapshotIdAccessor,
                                      sbe::value::SlotAccessor* indexIdAccessor,
                                      sbe::value::SlotAccessor* indexKeyAccessor,
                                      const CollectionPtr& collection,
                                      const Record& nextRecord) {
    if (snapshotIdAccessor) {
        auto currentSnapshotId = opCtx->recoveryUnit()->getSnapshotId();
        auto [snapshotIdTag, snapshotIdVal] = snapshotIdAccessor->getViewOfValue();
        const auto msgSnapshotIdTag = snapshotIdTag;
        tassert(5290704,
                str::stream() << "SnapshotId is of wrong type: " << msgSnapshotIdTag,
                snapshotIdTag == sbe::value::TypeTags::NumberInt64);

        auto snapshotId = sbe::value::bitcastTo<uint64_t>(snapshotIdVal);
        if (currentSnapshotId.toNumber() != snapshotId) {
            tassert(5290707, "Should have index key accessor", indexKeyAccessor);
            tassert(5290714, "Should have index id accessor", indexIdAccessor);

            auto [indexIdTag, indexIdVal] = indexIdAccessor->getViewOfValue();
            auto [ksTag, ksVal] = indexKeyAccessor->getViewOfValue();

            const auto msgIndexIdTag = indexIdTag;
            tassert(5290708,
                    str::stream() << "Index name is of wrong type: " << msgIndexIdTag,
                    sbe::value::isString(indexIdTag));

            const auto msgKsTag = ksTag;
            tassert(5290710,
                    str::stream() << "KeyString is of wrong type: " << msgKsTag,
                    ksTag == sbe::value::TypeTags::ksValue);

            auto keyString = sbe::value::getKeyStringView(ksVal);
            auto indexId = sbe::value::getStringView(indexIdTag, indexIdVal);
            tassert(5290712, "KeyString does not exist", keyString);

            auto it = iamTable.find(indexId);
            tassert(5290713,
                    str::stream() << "IndexAccessMethod not found for index " << indexId,
                    it != iamTable.end());

            auto iam = it->second;
            tassert(5290709,
                    str::stream() << "Expected to find IndexAccessMethod for index " << indexId,
                    iam);

            auto& executionCtx = StorageExecutionContext::get(opCtx);
            auto keys = executionCtx.keys();
            SharedBufferFragmentBuilder pooledBuilder(
                KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

            // There's no need to compute the prefixes of the indexed fields that cause the
            // index to be multikey when ensuring the keyData is still valid.
            KeyStringSet* multikeyMetadataKeys = nullptr;
            MultikeyPaths* multikeyPaths = nullptr;

            iam->getKeys(opCtx,
                         collection,
                         pooledBuilder,
                         nextRecord.data.toBson(),
                         IndexAccessMethod::GetKeysMode::kEnforceConstraints,
                         IndexAccessMethod::GetKeysContext::kValidatingKeys,
                         keys.get(),
                         multikeyMetadataKeys,
                         multikeyPaths,
                         nextRecord.id,
                         IndexAccessMethod::kNoopOnSuppressedErrorFn);

            return keys->count(*keyString);
        }
    }
    return true;
}

std::unique_ptr<fts::FTSMatcher> makeFtsMatcher(OperationContext* opCtx,
                                                const CollectionPtr& collection,
                                                const std::string& indexName,
                                                const fts::FTSQuery* ftsQuery) {
    auto desc = collection->getIndexCatalog()->findIndexByName(opCtx, indexName);
    tassert(5432209,
            str::stream() << "index descriptor not found for index named '" << indexName
                          << "' in collection '" << collection->ns() << "'",
            desc);

    auto entry = collection->getIndexCatalog()->getEntry(desc);
    tassert(5432210,
            str::stream() << "index entry not found for index named '" << indexName
                          << "' in collection '" << collection->ns() << "'",
            entry);

    auto accessMethod = static_cast<const FTSAccessMethod*>(entry->accessMethod());
    tassert(5432211,
            str::stream() << "access method is not defined for index named '" << indexName
                          << "' in collection '" << collection->ns() << "'",
            accessMethod);

    // We assume here that node->ftsQuery is an FTSQueryImpl, not an FTSQueryNoop. In practice, this
    // means that it is illegal to use the StageBuilder on a QuerySolution created by planning a
    // query that contains "no-op" expressions.
    auto query = dynamic_cast<const fts::FTSQueryImpl*>(ftsQuery);
    tassert(5432220, "expected FTSQueryImpl", query);
    return std::make_unique<fts::FTSMatcher>(*query, accessMethod->getSpec());
}
}  // namespace

SlotBasedStageBuilder::SlotBasedStageBuilder(OperationContext* opCtx,
                                             const CollectionPtr& collection,
                                             const CanonicalQuery& cq,
                                             const QuerySolution& solution,
                                             PlanYieldPolicySBE* yieldPolicy,
                                             ShardFiltererFactoryInterface* shardFiltererFactory)
    : StageBuilder(opCtx, collection, cq, solution),
      _yieldPolicy(yieldPolicy),
      _data(makeRuntimeEnvironment(_cq, _opCtx, &_slotIdGenerator)),
      _shardFiltererFactory(shardFiltererFactory),
      _state(_opCtx,
             _data.env,
             _cq.getExpCtxRaw()->variables,
             &_slotIdGenerator,
             &_frameIdGenerator,
             &_spoolIdGenerator) {
    // SERVER-52803: In the future if we need to gather more information from the QuerySolutionNode
    // tree, rather than doing one-off scans for each piece of information, we should add a formal
    // analysis pass here.
    // NOTE: Currently, we assume that each query operates on at most one collection, so there can
    // be only one STAGE_COLLSCAN node.
    if (auto node = getLoneNodeByType(solution.root(), STAGE_COLLSCAN)) {
        auto csn = static_cast<const CollectionScanNode*>(node);
        _data.shouldTrackLatestOplogTimestamp = csn->shouldTrackLatestOplogTimestamp;
        _data.shouldTrackResumeToken = csn->requestResumeToken;
        _data.shouldUseTailableScan = csn->tailable;
    }

    for (const auto& node : getAllNodesByType(solution.root(), STAGE_VIRTUAL_SCAN)) {
        auto vsn = static_cast<const VirtualScanNode*>(node);
        if (!vsn->hasRecordId) {
            _shouldProduceRecordIdSlot = false;
            break;
        }
    }
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::build(const QuerySolutionNode* root) {
    // For a given SlotBasedStageBuilder instance, this build() method can only be called once.
    invariant(!_buildHasStarted);
    _buildHasStarted = true;

    // We always produce a 'resultSlot' and conditionally produce a 'recordIdSlot' based on the
    // 'shouldProduceRecordIdSlot'.
    PlanStageReqs reqs;
    reqs.set(kResult);
    reqs.setIf(kRecordId, _shouldProduceRecordIdSlot);

    // Build the SBE plan stage tree.
    auto [stage, outputs] = build(root, reqs);

    // Assert that we produced a 'resultSlot' and that we prouced a 'recordIdSlot' if the
    // 'shouldProduceRecordIdSlot' flag was set. Also assert that we produced an 'oplogTsSlot' if
    // it's needed.
    invariant(outputs.has(kResult));
    invariant(!_shouldProduceRecordIdSlot || outputs.has(kRecordId));

    _data.outputs = std::move(outputs);

    return std::move(stage);
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildCollScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    invariant(!reqs.getIndexKeyBitset());

    auto csn = static_cast<const CollectionScanNode*>(root);

    auto [stage, outputs] = generateCollScan(
        _state, _collection, csn, _yieldPolicy, reqs.getIsTailableCollScanResumeBranch());

    if (reqs.has(kReturnKey)) {
        // Assign the 'returnKeySlot' to be the empty object.
        outputs.set(kReturnKey, _slotIdGenerator.generate());
        stage = sbe::makeProjectStage(std::move(stage),
                                      root->nodeId(),
                                      outputs.get(kReturnKey),
                                      sbe::makeE<sbe::EFunction>("newObj", sbe::makeEs()));
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildVirtualScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    using namespace std::literals;
    auto vsn = static_cast<const VirtualScanNode*>(root);
    // The caller should only have requested components of the index key if the virtual scan is
    // mocking an index scan.
    if (vsn->scanType == VirtualScanNode::ScanType::kCollScan) {
        invariant(!reqs.getIndexKeyBitset());
    }

    auto [inputTag, inputVal] = sbe::value::makeNewArray();
    sbe::value::ValueGuard inputGuard{inputTag, inputVal};
    auto inputView = sbe::value::getArrayView(inputVal);

    if (vsn->docs.size()) {
        inputView->reserve(vsn->docs.size());
        for (auto& doc : vsn->docs) {
            auto [tag, val] = makeValue(doc);
            inputView->push_back(tag, val);
        }
    }

    inputGuard.reset();
    auto [scanSlots, stage] =
        generateVirtualScanMulti(&_slotIdGenerator, vsn->hasRecordId ? 2 : 1, inputTag, inputVal);

    sbe::value::SlotId resultSlot;
    if (vsn->hasRecordId) {
        invariant(scanSlots.size() == 2);
        resultSlot = scanSlots[1];
    } else {
        invariant(scanSlots.size() == 1);
        resultSlot = scanSlots[0];
    }

    PlanStageSlots outputs;

    if (reqs.has(kResult)) {
        outputs.set(kResult, resultSlot);
    } else if (reqs.getIndexKeyBitset()) {
        // The caller wanted individual slots for certain components of a mock index scan. Use a
        // project stage to produce those slots. Since the test will represent index keys as BSON
        // objects, we use 'getField' expressions to extract the necessary fields.
        invariant(!vsn->indexKeyPattern.isEmpty());

        sbe::value::SlotVector indexKeySlots;
        sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projections;

        size_t indexKeyPos = 0;
        for (auto&& field : vsn->indexKeyPattern) {
            if (reqs.getIndexKeyBitset()->test(indexKeyPos)) {
                indexKeySlots.push_back(_slotIdGenerator.generate());
                projections.emplace(indexKeySlots.back(),
                                    makeFunction("getField"_sd,
                                                 sbe::makeE<sbe::EVariable>(resultSlot),
                                                 makeConstant(field.fieldName())));
            }
            ++indexKeyPos;
        }

        stage =
            sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projections), root->nodeId());

        outputs.setIndexKeySlots(indexKeySlots);
    }

    if (reqs.has(kRecordId)) {
        invariant(vsn->hasRecordId);
        invariant(scanSlots.size() == 2);
        outputs.set(kRecordId, scanSlots[0]);
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildIndexScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto ixn = static_cast<const IndexScanNode*>(root);
    invariant(reqs.has(kReturnKey) || !ixn->addKeyMetadata);

    sbe::IndexKeysInclusionSet indexKeyBitset;

    if (reqs.has(PlanStageSlots::kReturnKey) || reqs.has(PlanStageSlots::kResult)) {
        // If either 'reqs.result' or 'reqs.returnKey' is true, we need to get all parts of the
        // index key (regardless of what was requested by 'reqs.indexKeyBitset') so that we can
        // create the inflated index key (keyExpr).
        for (int i = 0; i < ixn->index.keyPattern.nFields(); ++i) {
            indexKeyBitset.set(i);
        }
    } else if (reqs.getIndexKeyBitset()) {
        indexKeyBitset = *reqs.getIndexKeyBitset();
    }

    // If the slots necessary for performing an index consistency check were not requested in
    // 'reqs', then don't pass a pointer to 'iamMap' so 'generateIndexScan' doesn't generate the
    // necessary slots.
    auto iamMap = &_data.iamMap;
    if (!(reqs.has(kSnapshotId) && reqs.has(kIndexId) && reqs.has(kIndexKey))) {
        iamMap = nullptr;
    }

    auto [stage, outputs] = generateIndexScan(
        _state, _collection, ixn, indexKeyBitset, _yieldPolicy, iamMap, reqs.has(kIndexKeyPattern));

    if (reqs.has(PlanStageSlots::kReturnKey)) {
        sbe::EExpression::Vector mkObjArgs;

        size_t i = 0;
        for (auto&& elem : ixn->index.keyPattern) {
            mkObjArgs.emplace_back(sbe::makeE<sbe::EConstant>(elem.fieldNameStringData()));
            mkObjArgs.emplace_back(sbe::makeE<sbe::EVariable>((*outputs.getIndexKeySlots())[i++]));
        }

        auto rawKeyExpr = sbe::makeE<sbe::EFunction>("newObj", std::move(mkObjArgs));
        outputs.set(PlanStageSlots::kReturnKey, _slotIdGenerator.generate());
        stage = sbe::makeProjectStage(std::move(stage),
                                      ixn->nodeId(),
                                      outputs.get(PlanStageSlots::kReturnKey),
                                      std::move(rawKeyExpr));
    }

    if (reqs.has(PlanStageSlots::kResult)) {
        outputs.set(PlanStageSlots::kResult, _slotIdGenerator.generate());
        stage = rehydrateIndexKey(std::move(stage),
                                  ixn->index.keyPattern,
                                  ixn->nodeId(),
                                  *outputs.getIndexKeySlots(),
                                  outputs.get(PlanStageSlots::kResult));
    }

    if (reqs.getIndexKeyBitset()) {
        outputs.setIndexKeySlots(
            makeIndexKeyOutputSlotsMatchingParentReqs(ixn->index.keyPattern,
                                                      *reqs.getIndexKeyBitset(),
                                                      indexKeyBitset,
                                                      *outputs.getIndexKeySlots()));
    } else {
        outputs.setIndexKeySlots(boost::none);
    }

    return {std::move(stage), std::move(outputs)};
}

std::tuple<sbe::value::SlotId, sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>>
SlotBasedStageBuilder::makeLoopJoinForFetch(std::unique_ptr<sbe::PlanStage> inputStage,
                                            sbe::value::SlotId seekKeySlot,
                                            sbe::value::SlotId snapshotIdSlot,
                                            sbe::value::SlotId indexIdSlot,
                                            sbe::value::SlotId indexKeySlot,
                                            sbe::value::SlotId indexKeyPatternSlot,
                                            StringMap<const IndexAccessMethod*> iamMap,
                                            PlanNodeId planNodeId,
                                            sbe::value::SlotVector slotsToForward) {
    auto resultSlot = _slotIdGenerator.generate();
    auto recordIdSlot = _slotIdGenerator.generate();

    using namespace std::placeholders;
    sbe::ScanCallbacks callbacks(
        indexKeyCorruptionCheckCallback,
        std::bind(indexKeyConsistencyCheckCallback, _1, std::move(iamMap), _2, _3, _4, _5, _6));
    // Scan the collection in the range [seekKeySlot, Inf).
    auto scanStage = sbe::makeS<sbe::ScanStage>(_collection->uuid(),
                                                resultSlot,
                                                recordIdSlot,
                                                snapshotIdSlot,
                                                indexIdSlot,
                                                indexKeySlot,
                                                indexKeyPatternSlot,
                                                boost::none,
                                                std::vector<std::string>{},
                                                sbe::makeSV(),
                                                seekKeySlot,
                                                true,
                                                nullptr,
                                                planNodeId,
                                                std::move(callbacks));

    // Get the recordIdSlot from the outer side (e.g., IXSCAN) and feed it to the inner side,
    // limiting the result set to 1 row.
    auto stage = sbe::makeS<sbe::LoopJoinStage>(
        std::move(inputStage),
        sbe::makeS<sbe::LimitSkipStage>(std::move(scanStage), 1, boost::none, planNodeId),
        std::move(slotsToForward),
        sbe::makeSV(seekKeySlot, snapshotIdSlot, indexIdSlot, indexKeySlot, indexKeyPatternSlot),
        nullptr,
        planNodeId);

    return {resultSlot, recordIdSlot, std::move(stage)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildFetch(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto fn = static_cast<const FetchNode*>(root);

    // The child must produce all of the slots required by the parent of this FetchNode, except for
    // 'resultSlot' which will be produced by the call to makeLoopJoinForFetch() below. In addition
    // to that, the child must always produce a 'recordIdSlot' because it's needed for the call to
    // makeLoopJoinForFetch() below.
    auto childReqs = reqs.copy()
                         .clear(kResult)
                         .set(kRecordId)
                         .set(kSnapshotId)
                         .set(kIndexId)
                         .set(kIndexKey)
                         .set(kIndexKeyPattern);

    auto [stage, outputs] = build(fn->children[0], childReqs);

    auto iamMap = _data.iamMap;
    uassert(4822880, "RecordId slot is not defined", outputs.has(kRecordId));
    uassert(
        4953600, "ReturnKey slot is not defined", !reqs.has(kReturnKey) || outputs.has(kReturnKey));
    uassert(5290701, "Snapshot id slot is not defined", outputs.has(kSnapshotId));
    uassert(5290702, "Index id slot is not defined", outputs.has(kIndexId));
    uassert(5290711, "Index key slot is not defined", outputs.has(kIndexKey));
    uassert(5113713, "Index key pattern slot is not defined", outputs.has(kIndexKeyPattern));

    auto forwardingReqs = reqs.copy().clear(kResult).clear(kRecordId);

    auto relevantSlots = sbe::makeSV();
    outputs.forEachSlot(forwardingReqs, [&](auto&& slot) { relevantSlots.push_back(slot); });

    // Forward slots for components of the index key if our parent requested them.
    if (auto indexKeySlots = outputs.getIndexKeySlots()) {
        relevantSlots.insert(relevantSlots.end(), indexKeySlots->begin(), indexKeySlots->end());
    }

    sbe::value::SlotId fetchResultSlot, fetchRecordIdSlot;
    std::tie(fetchResultSlot, fetchRecordIdSlot, stage) =
        makeLoopJoinForFetch(std::move(stage),
                             outputs.get(kRecordId),
                             outputs.get(kSnapshotId),
                             outputs.get(kIndexId),
                             outputs.get(kIndexKey),
                             outputs.get(kIndexKeyPattern),
                             std::move(iamMap),
                             root->nodeId(),
                             std::move(relevantSlots));

    outputs.set(kResult, fetchResultSlot);
    outputs.set(kRecordId, fetchRecordIdSlot);

    if (fn->filter) {
        forwardingReqs = reqs.copy().set(kResult).set(kRecordId);

        relevantSlots = sbe::makeSV();
        outputs.forEachSlot(forwardingReqs, [&](auto&& slot) { relevantSlots.push_back(slot); });

        // Forward slots for components of the index key if our parent requested them.
        if (auto indexKeySlots = outputs.getIndexKeySlots()) {
            relevantSlots.insert(relevantSlots.end(), indexKeySlots->begin(), indexKeySlots->end());
        }

        auto [_, outputStage] = generateFilter(_state,
                                               fn->filter.get(),
                                               {std::move(stage), std::move(relevantSlots)},
                                               outputs.get(kResult),
                                               root->nodeId());
        stage = std::move(outputStage.stage);
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildLimit(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto ln = static_cast<const LimitNode*>(root);
    boost::optional<long long> skip;

    auto [stage, outputs] = [&]() {
        if (ln->children[0]->getType() == StageType::STAGE_SKIP) {
            // If we have both limit and skip stages and the skip stage is beneath the limit, then
            // we can combine these two stages into one.
            const auto sn = static_cast<const SkipNode*>(ln->children[0]);
            skip = sn->skip;
            return build(sn->children[0], reqs);
        } else {
            return build(ln->children[0], reqs);
        }
    }();

    if (!reqs.getIsTailableCollScanResumeBranch()) {
        stage = std::make_unique<sbe::LimitSkipStage>(
            std::move(stage), ln->limit, skip, root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSkip(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto sn = static_cast<const SkipNode*>(root);
    auto [stage, outputs] = build(sn->children[0], reqs);

    if (!reqs.getIsTailableCollScanResumeBranch()) {
        stage = std::make_unique<sbe::LimitSkipStage>(
            std::move(stage), boost::none, sn->skip, root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

namespace {
using MakeSortKeyFn =
    std::function<std::unique_ptr<sbe::EExpression>(sbe::value::SlotId inputSlot)>;

/**
 * Given a field path, this function builds a plan stage tree that will produce the corresponding
 * sort key for that path. The 'makeSortKey' parameter is used to apply any transformations to the
 * leaf fields' values that are necessary (for example, calling collComparisonKey()).
 *
 * Note that when 'level' is 0, this function assumes that 'inputSlot' alrady contains the top-level
 * field value from the path, and thus it will forgo generating a call to getField(). When 'level'
 * is 1 or greater, this function will generate a call to getField() to read the field for that
 * level.
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateSortKeyTraversal(
    std::unique_ptr<sbe::PlanStage> inputStage,
    sbe::value::SlotId inputSlot,
    const FieldPath& fp,
    sbe::value::SortDirection direction,
    FieldIndex level,
    PlanNodeId planNodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    const MakeSortKeyFn& makeSortKey) {
    invariant(level < fp.getPathLength());

    const bool isLeafField = (level == fp.getPathLength() - 1u);

    auto [fieldSlot, fromBranch] = [&]() {
        if (level > 0) {
            // Generate a call to getField() to read the field at the current level and bind it to
            // 'fieldSlot'. According to MQL's sorting semantics, if the field doesn't exist we
            // should use Null as the sort key.
            auto getFieldExpr = makeFunction("getField"_sd,
                                             sbe::makeE<sbe::EVariable>(inputSlot),
                                             sbe::makeE<sbe::EConstant>(fp.getFieldName(level)));

            if (isLeafField) {
                // Wrapping the field access with makeFillEmptyNull() is only necessary for the
                // leaf field. For non-leaf fields, if the field doesn't exist then Nothing will
                // propagate through the TraverseStage and afterward it will be converted to Null
                // by a projection (see below).
                getFieldExpr = makeFillEmptyNull(std::move(getFieldExpr));
            }

            auto fieldSlot{slotIdGenerator->generate()};
            return std::make_pair(
                fieldSlot,
                sbe::makeProjectStage(
                    std::move(inputStage), planNodeId, fieldSlot, std::move(getFieldExpr)));
        }

        return std::make_pair(inputSlot, std::move(inputStage));
    }();

    // Generate the 'in' branch for the TraverseStage that we're about to construct.
    auto [innerSlot, innerBranch] = [&, fieldSlot = fieldSlot, &fromBranch = fromBranch]() {
        if (isLeafField) {
            // Base case: Genereate a ProjectStage to evaluate the predicate.
            auto innerSlot{slotIdGenerator->generate()};
            return std::make_pair(innerSlot,
                                  sbe::makeProjectStage(makeLimitCoScanTree(planNodeId),
                                                        planNodeId,
                                                        innerSlot,
                                                        makeSortKey(fieldSlot)));
        } else {
            // Recursive case.
            return generateSortKeyTraversal(makeLimitCoScanTree(planNodeId),
                                            fieldSlot,
                                            fp,
                                            direction,
                                            level + 1,
                                            planNodeId,
                                            slotIdGenerator,
                                            makeSortKey);
        }
    }();

    // Generate the traverse stage for the current nested level. The fold expression uses
    // well-ordered comparison (cmp3w) to produce the minimum element (if 'direction' is
    // Ascending) or the maximum element (if 'direction' is Descending).
    auto traverseSlot{slotIdGenerator->generate()};
    auto outputSlot{slotIdGenerator->generate()};
    auto op = (direction == sbe::value::SortDirection::Ascending) ? sbe::EPrimBinary::less
                                                                  : sbe::EPrimBinary::greater;

    auto outputStage = sbe::makeS<sbe::TraverseStage>(
        std::move(fromBranch),
        std::move(innerBranch),
        fieldSlot,
        traverseSlot,
        innerSlot,
        sbe::makeSV(),
        sbe::makeE<sbe::EIf>(makeBinaryOp(op,
                                          makeBinaryOp(sbe::EPrimBinary::cmp3w,
                                                       makeVariable(innerSlot),
                                                       makeVariable(traverseSlot)),
                                          makeConstant(sbe::value::TypeTags::NumberInt64,
                                                       sbe::value::bitcastFrom<int64_t>(0))),
                             makeVariable(innerSlot),
                             makeVariable(traverseSlot)),
        nullptr,
        planNodeId,
        1);

    // According to MQL's sorting semantics, when a leaf field is an empty array we should use
    // Undefined as the sort key, and when a non-leaf field is an empty array or doesn't exist
    // we should use Null as the sort key.
    return {outputSlot,
            sbe::makeProjectStage(std::move(outputStage),
                                  planNodeId,
                                  outputSlot,
                                  isLeafField ? makeFillEmptyUndefined(makeVariable(traverseSlot))
                                              : makeFillEmptyNull(makeVariable(traverseSlot)))};
}

/**
 * Given a field path, this function will return an expression that will be true if evaluating the
 * field path involves array traversal at any level of the path (including the leaf field).
 */
std::unique_ptr<sbe::EExpression> generateArrayCheckForSortHelper(
    std::unique_ptr<sbe::EExpression> inputExpr,
    const FieldPath& fp,
    FieldIndex level,
    sbe::value::FrameIdGenerator* frameIdGenerator) {
    invariant(level < fp.getPathLength());

    auto fieldExpr = makeFillEmptyNull(makeFunction(
        "getField"_sd, std::move(inputExpr), sbe::makeE<sbe::EConstant>(fp.getFieldName(level))));

    if (level == fp.getPathLength() - 1u) {
        return makeFunction("isArray"_sd, std::move(fieldExpr));
    } else {
        auto frameId = frameIdGenerator->generate();
        return sbe::makeE<sbe::ELocalBind>(
            frameId,
            sbe::makeEs(std::move(fieldExpr)),
            makeBinaryOp(sbe::EPrimBinary::logicOr,
                         makeFunction("isArray"_sd, makeVariable(frameId, 0)),
                         generateArrayCheckForSortHelper(
                             makeVariable(frameId, 0), fp, level + 1, frameIdGenerator)));
    }
}

/**
 * Given a field path and a slot that holds the top-level field's value from that path, this
 * function will return an expression that will be true if evaluating the field path involves array
 * traversal at any level of the path (including the leaf field).
 */
std::unique_ptr<sbe::EExpression> generateArrayCheckForSort(
    sbe::value::SlotId inputSlot,
    const FieldPath& fp,
    sbe::value::FrameIdGenerator* frameIdGenerator) {
    if (fp.getPathLength() == 1) {
        return makeFunction("isArray"_sd, makeVariable(inputSlot));
    } else {
        return makeBinaryOp(
            sbe::EPrimBinary::logicOr,
            makeFunction("isArray"_sd, makeVariable(inputSlot)),
            generateArrayCheckForSortHelper(makeVariable(inputSlot), fp, 1, frameIdGenerator));
    }
}
}  // namespace

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSort(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto sn = static_cast<const SortNode*>(root);
    auto sortPattern = SortPattern{sn->pattern, _cq.getExpCtx()};

    tassert(5037001,
            "QueryPlannerAnalysis should not produce a SortNode with an empty sort pattern",
            sortPattern.size() > 0);

    // The child must produce all of the slots required by the parent of this SortNode.
    auto childReqs = reqs.copy();
    auto child = sn->children[0];

    const auto isCoveredQuery = reqs.getIndexKeyBitset().has_value();
    BSONObj indexKeyPattern;
    sbe::IndexKeysInclusionSet sortPatternKeyBitSet;
    if (isCoveredQuery) {
        // If query is covered, we need to request index key for each part of the sort pattern.
        auto indexScan = static_cast<const IndexScanNode*>(getLoneNodeByType(child, STAGE_IXSCAN));
        tassert(5601701, "Expected index scan below sort for covered query", indexScan);
        indexKeyPattern = indexScan->index.keyPattern;

        StringDataSet sortPaths;
        for (const auto& part : sortPattern) {
            sortPaths.insert(part.fieldPath->fullPath());
        }

        std::vector<std::string> foundPaths;
        std::tie(sortPatternKeyBitSet, foundPaths) =
            makeIndexKeyInclusionSet(indexKeyPattern, sortPaths);
        *childReqs.getIndexKeyBitset() |= sortPatternKeyBitSet;
    } else {
        // If query is not covered, child is required to produce whole document for sorting.
        childReqs.set(kResult);
    }

    auto [inputStage, outputs] = build(child, childReqs);

    auto collatorSlot = _data.env->getSlotIfExists("collator"_sd);

    std::vector<sbe::value::SortDirection> direction;
    StringDataSet prefixSet;
    bool hasPartsWithCommonPrefix = false;

    for (const auto& part : sortPattern) {
        // getExecutor() should never call into buildSlotBasedExecutableTree() when the query
        // contains $meta, so this assertion should always be true.
        tassert(5037002, "Sort with $meta is not supported in SBE", part.fieldPath);

        if (!hasPartsWithCommonPrefix) {
            auto [_, prefixWasNotPresent] = prefixSet.insert(part.fieldPath->getFieldName(0));
            hasPartsWithCommonPrefix = !prefixWasNotPresent;
        }

        // Record the direction for this part of the sort pattern
        direction.push_back(part.isAscending ? sbe::value::SortDirection::Ascending
                                             : sbe::value::SortDirection::Descending);
    }

    sbe::value::SlotVector orderBy;
    orderBy.reserve(sortPattern.size());

    // Slots for sort stage to forward to parent stage. Values in these slots are not used during
    // sorting.
    auto forwardedSlots = sbe::makeSV();

    // We do not support covered queries on array fields for multikey indexes. This means that if
    // the query is covered, index keys cannot contain arrays. Since traversal logic and
    // 'generateSortKey' call below is needed only for arrays, we can omit it for covered queries.
    if (isCoveredQuery) {
        auto indexKeySlots = *outputs.extractIndexKeySlots();

        // Currently, 'indexKeySlots' contains slots for two kinds of index keys:
        //  1. Keys requested for sort pattern
        //  2. Keys requested by parent
        // We need to filter first category of slots into 'orderBy' vector, since sort stage will
        // use them for sorting. Second category of slots goes into 'outputs' to be used by parent.
        auto& parentIndexKeyBitset = *reqs.getIndexKeyBitset();
        auto& childIndexKeyBitset = *childReqs.getIndexKeyBitset();

        orderBy = makeIndexKeyOutputSlotsMatchingParentReqs(
            indexKeyPattern, sortPatternKeyBitSet, childIndexKeyBitset, indexKeySlots);

        auto indexKeySlotsForParent = makeIndexKeyOutputSlotsMatchingParentReqs(
            indexKeyPattern, parentIndexKeyBitset, childIndexKeyBitset, indexKeySlots);
        outputs.setIndexKeySlots(std::move(indexKeySlotsForParent));

        // In forwarded slots we need to include all slots requested by parent excluding slots from
        // 'orderBy' vector.
        auto forwardedIndexKeyBitset = parentIndexKeyBitset & (~sortPatternKeyBitSet);
        forwardedSlots = makeIndexKeyOutputSlotsMatchingParentReqs(indexKeyPattern,
                                                                   forwardedIndexKeyBitset,
                                                                   childIndexKeyBitset,
                                                                   std::move(indexKeySlots));
    } else if (!hasPartsWithCommonPrefix) {
        sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projectMap;

        for (const auto& part : sortPattern) {
            // Get the top-level field for this sort part. If the field doesn't exist, according to
            // MQL's sorting semantics we should use Null.
            auto getFieldExpr = makeFillEmptyNull(
                makeFunction("getField"_sd,
                             makeVariable(outputs.get(kResult)),
                             sbe::makeE<sbe::EConstant>(part.fieldPath->getFieldName(0))));

            auto fieldSlot{_slotIdGenerator.generate()};
            projectMap.emplace(fieldSlot, std::move(getFieldExpr));

            orderBy.push_back(fieldSlot);
        }

        inputStage = sbe::makeS<sbe::ProjectStage>(
            std::move(inputStage), std::move(projectMap), root->nodeId());

        auto failOnParallelArrays = [&]() -> std::unique_ptr<mongo::sbe::EExpression> {
            auto parallelArraysError = sbe::makeE<sbe::EFail>(
                ErrorCodes::BadValue, "cannot sort with keys that are parallel arrays");

            if (sortPattern.size() < 2) {
                // If the sort pattern only has one part, we don't need to generate a "parallel
                // arrays" check.
                return {};
            } else if (sortPattern.size() == 2) {
                // If the sort pattern has two parts, we can generate a simpler expression to
                // perform the "parallel arrays" check.
                auto makeIsNotArrayCheck = [&](sbe::value::SlotId slot, const FieldPath& fp) {
                    return makeNot(generateArrayCheckForSort(slot, fp, &_frameIdGenerator));
                };

                return makeBinaryOp(
                    sbe::EPrimBinary::logicOr,
                    makeIsNotArrayCheck(orderBy[0], *sortPattern[0].fieldPath),
                    makeBinaryOp(sbe::EPrimBinary::logicOr,
                                 makeIsNotArrayCheck(orderBy[1], *sortPattern[1].fieldPath),
                                 std::move(parallelArraysError)));
            } else {
                // If the sort pattern has three or more parts, we generate an expression to
                // perform the "parallel arrays" check that works (and scales well) for an
                // arbitrary number of sort pattern parts.
                auto makeIsArrayCheck = [&](sbe::value::SlotId slot, const FieldPath& fp) {
                    return makeBinaryOp(sbe::EPrimBinary::cmp3w,
                                        generateArrayCheckForSort(slot, fp, &_frameIdGenerator),
                                        makeConstant(sbe::value::TypeTags::Boolean, false));
                };

                auto numArraysExpr = makeIsArrayCheck(orderBy[0], *sortPattern[0].fieldPath);
                for (size_t idx = 1; idx < sortPattern.size(); ++idx) {
                    numArraysExpr =
                        makeBinaryOp(sbe::EPrimBinary::add,
                                     std::move(numArraysExpr),
                                     makeIsArrayCheck(orderBy[idx], *sortPattern[idx].fieldPath));
                }

                return makeBinaryOp(
                    sbe::EPrimBinary::logicOr,
                    makeBinaryOp(sbe::EPrimBinary::lessEq,
                                 std::move(numArraysExpr),
                                 makeConstant(sbe::value::TypeTags::NumberInt32, 1)),
                    std::move(parallelArraysError));
            }
        }();

        if (failOnParallelArrays) {
            inputStage = sbe::makeProjectStage(std::move(inputStage),
                                               root->nodeId(),
                                               _slotIdGenerator.generate(),
                                               std::move(failOnParallelArrays));
        }

        for (size_t idx = 0; idx < orderBy.size(); ++idx) {
            auto makeSortKey = [&](sbe::value::SlotId inputSlot) {
                return !collatorSlot ? makeVariable(inputSlot)
                                     : makeFunction("collComparisonKey"_sd,
                                                    makeVariable(inputSlot),
                                                    makeVariable(*collatorSlot));
            };

            // Call generateSortKeyTraversal() to build a series of TraverseStages that will
            // traverse this part's field path and produce the corresponding sort key. We pass
            // in the 'makeSortKey' lambda, which will be applied on each leaf field's value
            // to apply the current collation (if there is one).
            sbe::value::SlotId sortKeySlot;
            std::tie(sortKeySlot, inputStage) =
                generateSortKeyTraversal(std::move(inputStage),
                                         orderBy[idx],
                                         *sortPattern[idx].fieldPath,
                                         direction[idx],
                                         0,
                                         root->nodeId(),
                                         &_slotIdGenerator,
                                         makeSortKey);

            orderBy[idx] = sortKeySlot;
        }
    } else {
        // Handle the case where two or more parts of the sort pattern have a common prefix.
        orderBy = _slotIdGenerator.generateMultiple(1);
        direction = {sbe::value::SortDirection::Ascending};

        auto sortSpecExpr =
            makeConstant(sbe::value::TypeTags::sortSpec,
                         sbe::value::bitcastFrom<sbe::value::SortSpec*>(
                             new sbe::value::SortSpec(sn->pattern, _cq.getCollator())));

        inputStage = sbe::makeProjectStage(std::move(inputStage),
                                           root->nodeId(),
                                           orderBy[0],
                                           makeFunction("generateSortKey",
                                                        std::move(sortSpecExpr),
                                                        makeVariable(outputs.get(kResult))));
    }

    outputs.forEachSlot(childReqs, [&](auto&& slot) { forwardedSlots.push_back(slot); });

    inputStage =
        sbe::makeS<sbe::SortStage>(std::move(inputStage),
                                   std::move(orderBy),
                                   std::move(direction),
                                   std::move(forwardedSlots),
                                   sn->limit ? sn->limit : std::numeric_limits<std::size_t>::max(),
                                   sn->maxMemoryUsageBytes,
                                   _cq.getExpCtx()->allowDiskUse,
                                   root->nodeId());

    return {std::move(inputStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildSortKeyGeneraror(const QuerySolutionNode* root,
                                             const PlanStageReqs& reqs) {
    uasserted(4822883, "Sort key generator in not supported in SBE yet");
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSortMerge(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    using namespace std::literals;
    auto mergeSortNode = static_cast<const MergeSortNode*>(root);

    const auto sortPattern = SortPattern{mergeSortNode->sort, _cq.getExpCtx()};
    std::vector<sbe::value::SortDirection> direction;

    for (const auto& part : sortPattern) {
        uassert(4822881, "Sorting by expression not supported", !part.expression);
        direction.push_back(part.isAscending ? sbe::value::SortDirection::Ascending
                                             : sbe::value::SortDirection::Descending);
    }

    sbe::PlanStage::Vector inputStages;
    std::vector<sbe::value::SlotVector> inputKeys;
    std::vector<sbe::value::SlotVector> inputVals;

    // Children must produce all of the slots required by the parent of this SortMergeNode. In
    // addition, children must always produce a 'recordIdSlot' if the 'dedup' flag is true.
    auto childReqs = reqs.copy().setIf(kRecordId, mergeSortNode->dedup);

    // If a parent node has requested an index key bitset, then we will produce index keys
    // corresponding to the sort pattern parts needed by said parent node.
    bool parentRequestsIdxKeys = reqs.getIndexKeyBitset().has_value();

    for (auto&& child : mergeSortNode->children) {
        sbe::value::SlotVector inputKeysForChild;

        // Retrieve the sort pattern provided by the subtree rooted at 'child'. In particular, if
        // our child is a MERGE_SORT, it will provide the sort directly. If instead it's a tree
        // containing a lone IXSCAN, we have to check the key pattern because 'providedSorts()' may
        // or may not provide the baseSortPattern depending on the index bounds (in particular,
        // if the bounds are fixed, the fields will be marked as 'ignored'). Otherwise, we attempt
        // to retrieve it from 'providedSorts'.
        auto childSortPattern = [&]() {
            if (auto [msn, _] = getFirstNodeByType(child, STAGE_SORT_MERGE); msn) {
                auto node = static_cast<const MergeSortNode*>(msn);
                return node->sort;
            } else {
                auto [ixn, ct] = getFirstNodeByType(child, STAGE_IXSCAN);
                if (ixn && ct == 1) {
                    auto node = static_cast<const IndexScanNode*>(ixn);
                    return node->index.keyPattern;
                }
            }
            auto baseSort = child->providedSorts().getBaseSortPattern();
            tassert(6149600,
                    str::stream() << "Did not find sort pattern for child " << child->toString(),
                    !baseSort.isEmpty());
            return baseSort;
        }();

        // Map of field name to position within the index key. This is used to account for
        // mismatches between the sort pattern and the index key pattern. For instance, suppose
        // the requested sort is {a: 1, b: 1} and the index key pattern is {c: 1, b: 1, a: 1}.
        // When the slots for the relevant components of the index key are generated (i.e.
        // extract keys for 'b' and 'a'),  we wish to insert them into 'inputKeys' in the order
        // that they appear in the sort pattern.
        StringMap<size_t> indexKeyPositionMap;

        sbe::IndexKeysInclusionSet indexKeyBitset;
        size_t i = 0;
        for (auto&& elt : childSortPattern) {
            for (auto&& sortPart : sortPattern) {
                auto path = sortPart.fieldPath->fullPath();
                if (elt.fieldNameStringData() == path) {
                    indexKeyBitset.set(i);
                    indexKeyPositionMap.emplace(path, indexKeyPositionMap.size());
                    break;
                }
            }
            ++i;
        }
        childReqs.getIndexKeyBitset() = indexKeyBitset;

        // Children must produce a 'resultSlot' if they produce fetched results.
        auto [stage, outputs] = build(child, childReqs);

        tassert(5184301,
                "SORT_MERGE node must receive a RecordID slot as input from child stage"
                " if the 'dedup' flag is set",
                !mergeSortNode->dedup || outputs.has(kRecordId));

        // Clear the index key bitset after building the child stage.
        childReqs.getIndexKeyBitset() = boost::none;

        // Insert the index key slots in the order of the sort pattern.
        auto indexKeys = outputs.extractIndexKeySlots();
        tassert(5184302,
                "SORT_MERGE must receive index key slots as input from its child stages",
                indexKeys);

        for (const auto& part : sortPattern) {
            auto partPath = part.fieldPath->fullPath();
            auto index = indexKeyPositionMap.find(partPath);
            tassert(5184303,
                    str::stream() << "Could not find index key position for sort key part "
                                  << partPath,
                    index != indexKeyPositionMap.end());
            auto indexPos = index->second;
            tassert(5184304,
                    str::stream() << "Index position " << indexPos
                                  << " is not less than number of index components "
                                  << indexKeys->size(),
                    indexPos < indexKeys->size());
            auto indexKeyPart = indexKeys->at(indexPos);
            inputKeysForChild.push_back(indexKeyPart);
        }

        inputKeys.push_back(std::move(inputKeysForChild));
        inputStages.push_back(std::move(stage));

        auto sv = sbe::makeSV();
        outputs.forEachSlot(childReqs, [&](auto&& slot) { sv.push_back(slot); });

        // If the parent of 'root' has requested index keys, then we need to pass along our input
        // keys as input values, as they will be part of the output 'root' provides its parent.
        if (parentRequestsIdxKeys) {
            for (auto&& inputKey : inputKeys.back()) {
                sv.push_back(inputKey);
            }
        }
        inputVals.push_back(std::move(sv));
    }

    auto outputVals = sbe::makeSV();

    PlanStageSlots outputs(childReqs, &_slotIdGenerator);
    outputs.forEachSlot(childReqs, [&](auto&& slot) { outputVals.push_back(slot); });

    // If the parent of 'root' has requested index keys, then we need to generate output slots to
    // hold the index keys that will be used as input to the parent of 'root'.
    if (parentRequestsIdxKeys) {
        auto idxKeySv = sbe::makeSV();
        for (int idx = 0; idx < mergeSortNode->sort.nFields(); ++idx) {
            idxKeySv.emplace_back(_slotIdGenerator.generate());
        }
        outputs.setIndexKeySlots(idxKeySv);

        for (auto keySlot : idxKeySv) {
            outputVals.push_back(keySlot);
        }
    }

    auto stage = sbe::makeS<sbe::SortedMergeStage>(std::move(inputStages),
                                                   std::move(inputKeys),
                                                   std::move(direction),
                                                   std::move(inputVals),
                                                   std::move(outputVals),
                                                   root->nodeId());

    if (mergeSortNode->dedup) {
        stage = sbe::makeS<sbe::UniqueStage>(
            std::move(stage), sbe::makeSV(outputs.get(kRecordId)), root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionSimple(const QuerySolutionNode* root,
                                             const PlanStageReqs& reqs) {
    using namespace std::literals;
    invariant(!reqs.getIndexKeyBitset());

    auto pn = static_cast<const ProjectionNodeSimple*>(root);

    // The child must produce all of the slots required by the parent of this ProjectionNodeSimple.
    // In addition to that, the child must always produce a 'resultSlot' because it's needed by the
    // projection logic below.
    auto childReqs = reqs.copy().set(kResult);
    auto [inputStage, outputs] = build(pn->children[0], childReqs);

    const auto childResult = outputs.get(kResult);

    outputs.set(kResult, _slotIdGenerator.generate());
    inputStage = sbe::makeS<sbe::MakeBsonObjStage>(std::move(inputStage),
                                                   outputs.get(kResult),
                                                   childResult,
                                                   sbe::MakeBsonObjStage::FieldBehavior::keep,
                                                   pn->proj.getRequiredFields(),
                                                   std::vector<std::string>{},
                                                   sbe::value::SlotVector{},
                                                   true,
                                                   false,
                                                   root->nodeId());

    return {std::move(inputStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionCovered(const QuerySolutionNode* root,
                                              const PlanStageReqs& reqs) {
    using namespace std::literals;
    invariant(!reqs.getIndexKeyBitset());

    auto pn = static_cast<const ProjectionNodeCovered*>(root);
    invariant(pn->proj.isSimple());

    tassert(5037301,
            str::stream() << "Can't build covered projection for fetched sub-plan: "
                          << root->toString(),
            !pn->children[0]->fetched());

    // This is a ProjectionCoveredNode, so we will be pulling all the data we need from one index.
    // Prepare a bitset to indicate which parts of the index key we need for the projection.
    StringSet requiredFields = {pn->proj.getRequiredFields().begin(),
                                pn->proj.getRequiredFields().end()};

    // The child must produce all of the slots required by the parent of this ProjectionNodeSimple,
    // except for 'resultSlot' which will be produced by the MakeBsonObjStage below. In addition to
    // that, the child must produce the index key slots that are needed by this covered projection.
    //
    // pn->coveredKeyObj is the "index.keyPattern" from the child (which is either an IndexScanNode
    // or DistinctNode). pn->coveredKeyObj lists all the fields that the index can provide, not the
    // fields that the projection wants. requiredFields lists all of the fields that the projection
    // needs. Since this is a covered projection, we're guaranteed that pn->coveredKeyObj contains
    // all of the fields that the projection needs.
    auto childReqs = reqs.copy().clear(kResult);

    auto [indexKeyBitset, keyFieldNames] =
        makeIndexKeyInclusionSet(pn->coveredKeyObj, requiredFields);
    childReqs.getIndexKeyBitset() = std::move(indexKeyBitset);

    auto [inputStage, outputs] = build(pn->children[0], childReqs);

    // Assert that the index scan produced index key slots for this covered projection.
    auto indexKeySlots = *outputs.extractIndexKeySlots();

    outputs.set(kResult, _slotIdGenerator.generate());
    inputStage = sbe::makeS<sbe::MakeBsonObjStage>(std::move(inputStage),
                                                   outputs.get(kResult),
                                                   boost::none,
                                                   boost::none,
                                                   std::vector<std::string>{},
                                                   std::move(keyFieldNames),
                                                   std::move(indexKeySlots),
                                                   true,
                                                   false,
                                                   root->nodeId());

    return {std::move(inputStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionDefault(const QuerySolutionNode* root,
                                              const PlanStageReqs& reqs) {
    invariant(!reqs.getIndexKeyBitset());

    auto childReqs = reqs.copy();

    auto pn = static_cast<const ProjectionNodeDefault*>(root);
    const auto& projection = pn->proj;
    const auto [indexScanNode, indexScanCount] = getFirstNodeByType(root, STAGE_IXSCAN);
    // TODO SERVER-57533: Support multiple index scan nodes located below OR and SORT_MERGE stages.
    const auto isCoveredProjection =
        !pn->fetched() && indexScanCount == 1 && projection.isInclusionOnly();

    boost::optional<IndexKeyPatternTreeNode> patternRoot;
    std::vector<IndexKeyPatternTreeNode*> patternNodesForSlots;
    if (isCoveredProjection) {
        // Convert projection fieldpaths into the tree of 'IndexKeyPatternTreeNode'.
        IndexKeysBuilderContext context;
        IndexKeysPreBuilder preVisitor{&context};
        IndexKeysInBuilder inVisitor{&context};
        IndexKeysPostBuilder postVisitor{&context};
        projection_ast::ProjectionASTConstWalker walker{&preVisitor, &inVisitor, &postVisitor};
        tree_walker::walk<true, projection_ast::ASTNode>(projection.root(), &walker);
        patternRoot = std::move(context.root);

        // Construct a bitset requesting slots from the underlying index scan. These slots
        // correspond to index keys for projection fieldpaths.
        auto& indexKeyPattern = static_cast<const IndexScanNode*>(indexScanNode)->index.keyPattern;
        size_t i = 0;
        sbe::IndexKeysInclusionSet patternBitSet;
        for (const auto& element : indexKeyPattern) {
            FieldRef fieldRef{element.fieldNameStringData()};
            // Projection field paths are always leaf nodes. In other words, projection like
            // {a: 1, 'a.b': 1} would produce a path collision error.
            if (auto node = patternRoot->findLeafNode(fieldRef); node) {
                patternBitSet.set(i);
                patternNodesForSlots.push_back(node);
            }

            ++i;
        }

        childReqs.getIndexKeyBitset() = patternBitSet;

        // We do not need index scan to restore the entire object. Instead, we will restore only
        // necessary parts of it below.
        childReqs.clear(kResult);
    } else {
        // The child must produce all of the slots required by the parent of this
        // ProjectionNodeDefault. In addition to that, the child must always produce a 'resultSlot'
        // because it's needed by the projection logic below.
        childReqs.set(kResult);
    }

    auto [inputStage, outputs] = build(pn->children[0], childReqs);

    sbe::value::SlotId resultSlot;
    std::unique_ptr<sbe::PlanStage> resultStage;
    if (isCoveredProjection) {
        auto indexKeySlots = *outputs.extractIndexKeySlots();

        // Extract slots corresponding to each of the projection fieldpaths.
        invariant(indexKeySlots.size() == patternNodesForSlots.size());
        for (size_t i = 0; i < indexKeySlots.size(); i++) {
            patternNodesForSlots[i]->indexKeySlot = indexKeySlots[i];
        }

        // Finally, build the expression to create object with requested projection fieldpaths.
        resultSlot = _slotIdGenerator.generate();
        auto resultExpr = buildNewObjExpr(&*patternRoot);
        resultStage = sbe::makeProjectStage(
            std::move(inputStage), root->nodeId(), resultSlot, std::move(resultExpr));
    } else {
        auto relevantSlots = sbe::makeSV();
        outputs.forEachSlot(childReqs, [&](auto&& slot) { relevantSlots.push_back(slot); });

        EvalStage stage;
        std::tie(resultSlot, stage) =
            generateProjection(_state,
                               &projection,
                               {std::move(inputStage), std::move(relevantSlots)},
                               outputs.get(kResult),
                               root->nodeId());

        resultStage = std::move(stage.stage);
    }

    outputs.set(kResult, resultSlot);
    return {std::move(resultStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildOr(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    invariant(!reqs.getIndexKeyBitset());

    sbe::PlanStage::Vector inputStages;
    std::vector<sbe::value::SlotVector> inputSlots;

    auto orn = static_cast<const OrNode*>(root);

    // Children must produce all of the slots required by the parent of this OrNode. In addition
    // to that, children must always produce a 'recordIdSlot' if the 'dedup' flag is true, and
    // children must always produce a 'resultSlot' if 'filter' is non-null.
    auto childReqs = reqs.copy().setIf(kResult, orn->filter.get()).setIf(kRecordId, orn->dedup);

    for (auto&& child : orn->children) {
        auto [stage, outputs] = build(child, childReqs);

        auto sv = sbe::makeSV();
        outputs.forEachSlot(childReqs, [&](auto&& slot) { sv.push_back(slot); });

        inputStages.push_back(std::move(stage));
        inputSlots.emplace_back(std::move(sv));
    }

    // Construct a union stage whose branches are translated children of the 'Or' node.
    auto unionOutputSlots = sbe::makeSV();

    PlanStageSlots outputs(childReqs, &_slotIdGenerator);
    outputs.forEachSlot(childReqs, [&](auto&& slot) { unionOutputSlots.push_back(slot); });

    auto stage = sbe::makeS<sbe::UnionStage>(
        std::move(inputStages), std::move(inputSlots), std::move(unionOutputSlots), root->nodeId());

    if (orn->dedup) {
        stage = sbe::makeS<sbe::UniqueStage>(
            std::move(stage), sbe::makeSV(outputs.get(kRecordId)), root->nodeId());
    }

    if (orn->filter) {
        auto forwardingReqs = reqs.copy().set(kResult);

        auto relevantSlots = sbe::makeSV();
        outputs.forEachSlot(forwardingReqs, [&](auto&& slot) { relevantSlots.push_back(slot); });

        auto [_, outputStage] = generateFilter(_state,
                                               orn->filter.get(),
                                               {std::move(stage), std::move(relevantSlots)},
                                               outputs.get(kResult),
                                               root->nodeId());
        stage = std::move(outputStage.stage);
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildTextMatch(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(5432212, "no collection object", _collection);
    tassert(5432213, "index keys requsted for text match node", !reqs.getIndexKeyBitset());
    tassert(5432215,
            str::stream() << "text match node must have one child, but got "
                          << root->children.size(),
            root->children.size() == 1);
    // TextMatchNode guarantees to produce a fetched sub-plan, but it doesn't fetch itself. Instead,
    // its child sub-plan must be fully fetched, and a text match plan is constructed under this
    // assumption.
    tassert(5432216, "text match input must be fetched", root->children[0]->fetched());

    auto textNode = static_cast<const TextMatchNode*>(root);

    auto childReqs = reqs.copy().set(kResult);
    auto [stage, outputs] = build(textNode->children[0], childReqs);
    tassert(5432217, "result slot is not produced by text match sub-plan", outputs.has(kResult));

    // Create an FTS 'matcher' to apply 'ftsQuery' to matching documents.
    auto matcher = makeFtsMatcher(
        _opCtx, _collection, textNode->index.identifier.catalogName, textNode->ftsQuery.get());

    // Build an 'ftsMatch' expression to match a document stored in the 'kResult' slot using the
    // 'matcher' instance.
    auto ftsMatch =
        makeFunction("ftsMatch",
                     makeConstant(sbe::value::TypeTags::ftsMatcher,
                                  sbe::value::bitcastFrom<fts::FTSMatcher*>(matcher.release())),
                     makeVariable(outputs.get(kResult)));

    // Wrap the 'ftsMatch' expression into an 'if' expression to ensure that it can be applied only
    // to a document.
    auto filter =
        sbe::makeE<sbe::EIf>(makeFunction("isObject", makeVariable(outputs.get(kResult))),
                             std::move(ftsMatch),
                             sbe::makeE<sbe::EFail>(ErrorCodes::Error{4623400},
                                                    "textmatch requires input to be an object"));

    // Add a filter stage to apply 'ftsQuery' to matching documents and discard documents which do
    // not match.
    stage =
        sbe::makeS<sbe::FilterStage<false>>(std::move(stage), std::move(filter), root->nodeId());

    if (reqs.has(kReturnKey)) {
        // Assign the 'returnKeySlot' to be the empty object.
        outputs.set(kReturnKey, _slotIdGenerator.generate());
        stage = sbe::makeProjectStage(
            std::move(stage), root->nodeId(), outputs.get(kReturnKey), makeFunction("newObj"));
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildReturnKey(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    invariant(!reqs.getIndexKeyBitset());

    // TODO SERVER-49509: If the projection includes {$meta: "sortKey"}, the result of this stage
    // should also include the sort key. Everything else in the projection is ignored.
    auto returnKeyNode = static_cast<const ReturnKeyNode*>(root);

    // The child must produce all of the slots required by the parent of this ReturnKeyNode except
    // for 'resultSlot'. In addition to that, the child must always produce a 'returnKeySlot'.
    // After build() returns, we take the 'returnKeySlot' produced by the child and store it into
    // 'resultSlot' for the parent of this ReturnKeyNode to consume.
    auto childReqs = reqs.copy().clear(kResult).set(kReturnKey);
    auto [stage, outputs] = build(returnKeyNode->children[0], childReqs);

    outputs.set(kResult, outputs.get(kReturnKey));
    outputs.clear(kReturnKey);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildEof(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    return generateEofPlan(root->nodeId(), reqs, &_slotIdGenerator);
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildAndHash(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto andHashNode = static_cast<const AndHashNode*>(root);

    tassert(5073711, "need at least two children for AND_HASH", andHashNode->children.size() >= 2);

    auto childReqs = reqs.copy().set(kResult).set(kRecordId);

    auto outerChild = andHashNode->children[0];
    auto innerChild = andHashNode->children[1];

    auto [outerStage, outerOutputs] = build(outerChild, childReqs);
    auto outerIdSlot = outerOutputs.get(kRecordId);
    auto outerResultSlot = outerOutputs.get(kResult);
    auto outerCondSlots = sbe::makeSV(outerIdSlot);
    auto outerProjectSlots = sbe::makeSV(outerResultSlot);

    auto [innerStage, innerOutputs] = build(innerChild, childReqs);
    tassert(5073712, "innerOutputs must contain kRecordId slot", innerOutputs.has(kRecordId));
    tassert(5073713, "innerOutputs must contain kResult slot", innerOutputs.has(kResult));
    auto innerIdSlot = innerOutputs.get(kRecordId);
    auto innerResultSlot = innerOutputs.get(kResult);
    auto innerSnapshotIdSlot = innerOutputs.getIfExists(kSnapshotId);
    auto innerIndexIdSlot = innerOutputs.getIfExists(kIndexId);
    auto innerIndexKeySlot = innerOutputs.getIfExists(kIndexKey);
    auto innerIndexKeyPatternSlot = innerOutputs.getIfExists(kIndexKeyPattern);

    auto innerCondSlots = sbe::makeSV(innerIdSlot);
    auto innerProjectSlots = sbe::makeSV(innerResultSlot);

    auto collatorSlot = _data.env->getSlotIfExists("collator"_sd);

    // Designate outputs.
    PlanStageSlots outputs(reqs, &_slotIdGenerator);
    if (reqs.has(kRecordId)) {
        outputs.set(kRecordId, innerIdSlot);
    }
    if (reqs.has(kResult)) {
        outputs.set(kResult, innerResultSlot);
    }
    if (reqs.has(kSnapshotId) && innerSnapshotIdSlot) {
        auto slot = *innerSnapshotIdSlot;
        innerProjectSlots.push_back(slot);
        outputs.set(kSnapshotId, slot);
    }
    if (reqs.has(kIndexId) && innerIndexIdSlot) {
        auto slot = *innerIndexIdSlot;
        innerProjectSlots.push_back(slot);
        outputs.set(kIndexId, slot);
    }
    if (reqs.has(kIndexKey) && innerIndexKeySlot) {
        auto slot = *innerIndexKeySlot;
        innerProjectSlots.push_back(slot);
        outputs.set(kIndexKey, slot);
    }

    if (reqs.has(kIndexKeyPattern) && innerIndexKeyPatternSlot) {
        auto slot = *innerIndexKeyPatternSlot;
        innerProjectSlots.push_back(slot);
        outputs.set(kIndexKeyPattern, slot);
    }

    auto hashJoinStage = sbe::makeS<sbe::HashJoinStage>(std::move(outerStage),
                                                        std::move(innerStage),
                                                        outerCondSlots,
                                                        outerProjectSlots,
                                                        innerCondSlots,
                                                        innerProjectSlots,
                                                        collatorSlot,
                                                        root->nodeId());

    // If there are more than 2 children, iterate all remaining children and hash
    // join together.
    for (size_t i = 2; i < andHashNode->children.size(); i++) {
        auto [stage, outputs] = build(andHashNode->children[i], childReqs);
        tassert(5073714, "outputs must contain kRecordId slot", outputs.has(kRecordId));
        tassert(5073715, "outputs must contain kResult slot", outputs.has(kResult));
        auto idSlot = outputs.get(kRecordId);
        auto resultSlot = outputs.get(kResult);
        auto condSlots = sbe::makeSV(idSlot);
        auto projectSlots = sbe::makeSV(resultSlot);

        // The previous HashJoinStage is always set as the inner stage, so that we can reuse the
        // innerIdSlot and innerResultSlot that have been designated as outputs.
        hashJoinStage = sbe::makeS<sbe::HashJoinStage>(std::move(stage),
                                                       std::move(hashJoinStage),
                                                       condSlots,
                                                       projectSlots,
                                                       innerCondSlots,
                                                       innerProjectSlots,
                                                       collatorSlot,
                                                       root->nodeId());
    }

    return {std::move(hashJoinStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildAndSorted(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto andSortedNode = static_cast<const AndSortedNode*>(root);

    // Need at least two children.
    tassert(
        5073706, "need at least two children for AND_SORTED", andSortedNode->children.size() >= 2);

    auto childReqs = reqs.copy().set(kResult).set(kRecordId);

    auto outerChild = andSortedNode->children[0];
    auto innerChild = andSortedNode->children[1];

    auto [outerStage, outerOutputs] = build(outerChild, childReqs);
    auto outerIdSlot = outerOutputs.get(kRecordId);
    auto outerResultSlot = outerOutputs.get(kResult);

    auto outerKeySlots = sbe::makeSV(outerIdSlot);
    auto outerProjectSlots = sbe::makeSV(outerResultSlot);
    if (outerOutputs.has(kSnapshotId)) {
        outerProjectSlots.push_back(outerOutputs.get(kSnapshotId));
    }

    if (outerOutputs.has(kIndexId)) {
        outerProjectSlots.push_back(outerOutputs.get(kIndexId));
    }

    if (outerOutputs.has(kIndexKey)) {
        outerProjectSlots.push_back(outerOutputs.get(kIndexKey));
    }

    if (outerOutputs.has(kIndexKeyPattern)) {
        outerProjectSlots.push_back(outerOutputs.get(kIndexKeyPattern));
    }

    auto [innerStage, innerOutputs] = build(innerChild, childReqs);
    tassert(5073707, "innerOutputs must contain kRecordId slot", innerOutputs.has(kRecordId));
    tassert(5073708, "innerOutputs must contain kResult slot", innerOutputs.has(kResult));
    auto innerIdSlot = innerOutputs.get(kRecordId);
    auto innerResultSlot = innerOutputs.get(kResult);

    auto innerKeySlots = sbe::makeSV(innerIdSlot);
    auto innerProjectSlots = sbe::makeSV(innerResultSlot);

    PlanStageSlots outputs(reqs, &_slotIdGenerator);
    if (reqs.has(kRecordId)) {
        outputs.set(kRecordId, innerIdSlot);
    }
    if (reqs.has(kResult)) {
        outputs.set(kResult, innerResultSlot);
    }
    if (reqs.has(kSnapshotId)) {
        auto innerSnapshotSlot = innerOutputs.get(kSnapshotId);
        innerProjectSlots.push_back(innerSnapshotSlot);
        outputs.set(kSnapshotId, innerSnapshotSlot);
    }
    if (reqs.has(kIndexId)) {
        auto innerIndexIdSlot = innerOutputs.get(kIndexId);
        innerProjectSlots.push_back(innerIndexIdSlot);
        outputs.set(kIndexId, innerIndexIdSlot);
    }
    if (reqs.has(kIndexKey)) {
        auto innerIndexKeySlot = innerOutputs.get(kIndexKey);
        innerProjectSlots.push_back(innerIndexKeySlot);
        outputs.set(kIndexKey, innerIndexKeySlot);
    }

    if (reqs.has(kIndexKeyPattern)) {
        auto innerIndexKeyPatternSlot = innerOutputs.get(kIndexKeyPattern);
        innerProjectSlots.push_back(innerIndexKeyPatternSlot);
        outputs.set(kIndexKeyPattern, innerIndexKeyPatternSlot);
    }

    std::vector<sbe::value::SortDirection> sortDirs(outerKeySlots.size(),
                                                    sbe::value::SortDirection::Ascending);

    auto mergeJoinStage = sbe::makeS<sbe::MergeJoinStage>(std::move(outerStage),
                                                          std::move(innerStage),
                                                          outerKeySlots,
                                                          outerProjectSlots,
                                                          innerKeySlots,
                                                          innerProjectSlots,
                                                          sortDirs,
                                                          root->nodeId());

    // If there are more than 2 children, iterate all remaining children and merge
    // join together.
    for (size_t i = 2; i < andSortedNode->children.size(); i++) {
        auto [stage, outputs] = build(andSortedNode->children[i], childReqs);
        tassert(5073709, "outputs must contain kRecordId slot", outputs.has(kRecordId));
        tassert(5073710, "outputs must contain kResult slot", outputs.has(kResult));
        auto idSlot = outputs.get(kRecordId);
        auto resultSlot = outputs.get(kResult);
        auto keySlots = sbe::makeSV(idSlot);
        auto projectSlots = sbe::makeSV(resultSlot);

        mergeJoinStage = sbe::makeS<sbe::MergeJoinStage>(std::move(stage),
                                                         std::move(mergeJoinStage),
                                                         keySlots,
                                                         projectSlots,
                                                         innerKeySlots,
                                                         innerProjectSlots,
                                                         sortDirs,
                                                         root->nodeId());
    }

    return {std::move(mergeJoinStage), std::move(outputs)};
}

namespace {
const size_t kGroupBySlots = 1;

std::pair<sbe::value::SlotId, EvalStage> generateGroupByKey(
    StageBuilderState& state,
    Expression* idExpr,
    EvalStage childEvalStage,
    PlanNodeId nodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    // TODO SERVER-59951: make object form of the '_id' group-by expression work to handle multiple
    // group-by keys.
    auto rootSlot = childEvalStage.outSlots[0];
    auto [groupByEvalExpr, groupByEvalStage] = stage_builder::generateExpression(
        state, idExpr, std::move(childEvalStage), rootSlot, nodeId);

    if (auto isConstIdExpr = dynamic_cast<ExpressionConstant*>(idExpr) != nullptr; isConstIdExpr) {
        return projectEvalExpr(
            std::move(groupByEvalExpr), std::move(groupByEvalStage), nodeId, slotIdGenerator);
    } else {
        // The group-by field may end up being 'Nothing' and in that case _id: null will be
        // returned. Calling 'makeFillEmptyNull' for the group-by field takes care of that.
        return projectEvalExpr(makeFillEmptyNull(groupByEvalExpr.extractExpr()),
                               std::move(groupByEvalStage),
                               nodeId,
                               slotIdGenerator);
    }
}

std::tuple<sbe::value::SlotVector, EvalStage> generateAccumulator(
    StageBuilderState& state,
    const AccumulationStatement& accStmt,
    EvalStage childEvalStage,
    sbe::value::SlotId rootSlot,
    PlanNodeId nodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>>& accSlotToExprMap) {
    // Input fields may need field traversal which ends up being a complex tree.
    auto [argExpr, accArgEvalStage] =
        stage_builder::buildArgument(state, accStmt, std::move(childEvalStage), rootSlot, nodeId);

    // One accumulator may be translated to multiple accumulator expressions. For example, The
    // $avg will have two accumulators expressions, a sum(..) and a count which is implemented
    // as sum(1).
    auto [accExprs, accProjEvalStage] = stage_builder::buildAccumulator(
        state, accStmt, std::move(accArgEvalStage), std::move(argExpr), nodeId);

    sbe::value::SlotVector aggSlots;
    for (auto& accExpr : accExprs) {
        auto slot = slotIdGenerator->generate();
        aggSlots.push_back(slot);
        accSlotToExprMap.emplace(slot, std::move(accExpr));
    }

    return {std::move(aggSlots), std::move(accProjEvalStage)};
}

std::tuple<std::vector<std::string>, sbe::value::SlotVector, EvalStage> generateGroupFinalStage(
    StageBuilderState& state,
    EvalStage groupEvalStage,
    const std::vector<AccumulationStatement>& accStmts,
    sbe::value::SlotId groupBySlot,
    const std::vector<sbe::value::SlotVector>& aggSlotsVec,
    PlanNodeId nodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> prjSlotToExprMap;
    sbe::value::SlotVector groupOutSlots{groupEvalStage.outSlots};
    // To passthrough the output slots of accumulators with trivial finalizers, we need to find
    // their slot ids. We can do this by sorting 'groupEvalStage.outSlots' because the slot ids
    // correspond to the order in which the accumulators were translated (that is, the order in
    // which they are listed in 'accStmts'). Note, that 'groupEvalStage.outSlots' contains groupBy
    // slots at the front and the accumulator slots at the back.
    std::sort(groupOutSlots.begin() + kGroupBySlots, groupOutSlots.end());

    auto finalSlots{sbe::value::SlotVector{groupBySlot}};
    finalSlots.reserve(kGroupBySlots + accStmts.size());
    std::vector<std::string> fieldNames{"_id"};
    fieldNames.reserve(kGroupBySlots + accStmts.size());
    auto groupFinalEvalStage = std::move(groupEvalStage);
    size_t idxAccFirstSlot = kGroupBySlots;
    for (size_t idxAcc = 0; idxAcc < accStmts.size(); ++idxAcc) {
        // Gathers field names for the output object from accumulator statements.
        fieldNames.push_back(accStmts[idxAcc].fieldName);
        auto [finalExpr, tempEvalStage] = stage_builder::buildFinalize(
            state, accStmts[idxAcc], aggSlotsVec[idxAcc], std::move(groupFinalEvalStage), nodeId);

        // The final step may not return an expression if it's trivial. For example, $first and
        // $last's final steps are trivial.
        if (finalExpr) {
            auto outSlot = slotIdGenerator->generate();
            finalSlots.push_back(outSlot);
            prjSlotToExprMap.emplace(outSlot, std::move(finalExpr));
        } else {
            finalSlots.push_back(groupOutSlots[idxAccFirstSlot]);
        }

        // Some accumulator(s) like $avg generate multiple expressions and slots. So, need to
        // advance this index by the number of those slots for each accumulator.
        idxAccFirstSlot += aggSlotsVec[idxAcc].size();

        groupFinalEvalStage = std::move(tempEvalStage);
    }

    // Gathers all accumulator results. If there're no project expressions, does not add a project
    // stage.
    auto retEvalStage = prjSlotToExprMap.empty()
        ? std::move(groupFinalEvalStage)
        : makeProject(std::move(groupFinalEvalStage), std::move(prjSlotToExprMap), nodeId);

    return {std::move(fieldNames), std::move(finalSlots), std::move(retEvalStage)};
}
}  // namespace

/**
 * Translates a 'GroupNode' QSN into a sbe::PlanStage tree. This translation logic assumes that the
 * only child of the 'GroupNode' must return an Object (or 'BSONObject') and the translated sub-tree
 * must return 'BSONObject'. The returned 'BSONObject' will always have an "_id" field for the group
 * key and zero or more field(s) for accumulators.
 *
 * For example, a QSN tree: GroupNode(nodeId=2) over a VirtualScanNode(nodeId=1), we would have the
 * following translated sbe::PlanStage tree. In this example, we assume that the $group pipeline
 * spec is {"_id": "$a", "x": {"$min": "$b"}, "y": {"$first": "$b"}}.
 *
 * [2] mkbson s14 [_id = s9, x = s14, y = s13] true false
 * [2] project [s14 = fillEmpty (s11, null)]
 * [2] group [s9] [s12 = min (if (! exists (s9) || typeMatch (s9, 0x00000440), Nothing, s9)),
 *                 s13 = first (fillEmpty (s10, null))]
 * [2] project [s11 = getField (s7, "b")]
 * [2] project [s10 = getField (s7, "b")]
 * [2] project [s9 = fillEmpty (s8, null)]
 * [2] project [s8 = getField (s7, "a")]
 * [1] project [s7 = getElement (s5, 0)]
 * [1] unwind s5 s6 s4 false
 * [1] project [s4 = [[{"a" : 1, "b" : 1}], [{"a" : 1, "b" : 2}], [{"a" : 2, "b" : 3}]]]
 * [1] limit 1 [1] coscan
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildGroup(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    using namespace fmt::literals;

    auto groupNode = static_cast<const GroupNode*>(root);
    auto nodeId = groupNode->nodeId();
    const auto& idExpr = groupNode->groupByExpressions;

    tassert(
        5851600, "should have one and only one child for GROUP", groupNode->children.size() == 1);
    tassert(5851601, "{} slot should have been requested"_format(kResult), reqs.has(kResult));

    // If _id expression produces a scalar value, groupByExpressions must have only one element for
    // "_id" field. Otherwise, _id expression produces a document.
    //
    // Supports scalar _id expression only for now.
    uassert(5851602,
            "GROUP does not support document _id expression",
            idExpr.size() == kGroupBySlots && idExpr.contains("_id"));

    // Builds the child and gets the child result slot.
    auto [childStage, childOutputs] = build(groupNode->children[0], reqs);
    auto childResult = childOutputs.get(kResult);
    EvalStage childEvalStage{std::move(childStage), {childResult}};
    _shouldProduceRecordIdSlot = false;

    // Translates the group-by expression and binds it to a slot in a project stage.
    auto [groupBySlot, groupByEvalStage] = generateGroupByKey(
        _state, idExpr.at("_id").get(), std::move(childEvalStage), nodeId, &_slotIdGenerator);

    // Translates accumulators which are executed inside the group stage and gets slots for
    // accumulators.
    const auto& accStmts = groupNode->accumulators;
    stage_builder::EvalStage accProjEvalStage = std::move(groupByEvalStage);
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> accSlotToExprMap;
    std::vector<sbe::value::SlotVector> aggSlotsVec;
    for (const auto& accStmt : accStmts) {
        auto [aggSlots, tempEvalStage] = generateAccumulator(_state,
                                                             accStmt,
                                                             std::move(accProjEvalStage),
                                                             childResult,
                                                             nodeId,
                                                             &_slotIdGenerator,
                                                             accSlotToExprMap);
        aggSlotsVec.emplace_back(std::move(aggSlots));
        accProjEvalStage = std::move(tempEvalStage);
    }

    // Builds a group stage with accumulator expressions and a group-by slot.
    auto groupEvalStage = makeHashAgg(std::move(accProjEvalStage),
                                      {groupBySlot},
                                      std::move(accSlotToExprMap),
                                      _state.env->getSlotIfExists("collator"_sd),
                                      nodeId);

    tassert(
        5851603,
        "Group stage's output slots must include slots for group-by keys and slots for all "
        "accumulators",
        groupEvalStage.outSlots.size() ==
            std::accumulate(aggSlotsVec.begin(),
                            aggSlotsVec.end(),
                            kGroupBySlots,
                            [](int sum, const auto& aggSlots) { return sum + aggSlots.size(); }));
    tassert(5851604,
            "Group stage's output slots must contain the groupBySlot at the front",
            groupEvalStage.outSlots[0] == groupBySlot);

    // Builds the final stage(s) over the collected accumulators.
    auto [fieldNames, finalSlots, groupFinalEvalStage] =
        generateGroupFinalStage(_state,
                                std::move(groupEvalStage),
                                accStmts,
                                groupBySlot,
                                aggSlotsVec,
                                nodeId,
                                &_slotIdGenerator);

    tassert(5851605,
            "The number of final slots must be as the number of group-by slots + the number of acc "
            "slots",
            finalSlots.size() == kGroupBySlots + accStmts.size());

    // Builds a stage to create a result object out of a group-by slot and gathered accumulator
    // result slots.
    PlanStageSlots outputs;
    outputs.set(kResult, _slotIdGenerator.generate());
    // This mkbson stage combines 'finalSlots' into a bsonObject result slot which has 'fieldNames'
    // fields.
    auto outStage = sbe::makeS<sbe::MakeBsonObjStage>(std::move(groupFinalEvalStage.stage),
                                                      outputs.get(kResult),        // objSlot
                                                      boost::none,                 // rootSlot
                                                      boost::none,                 // fieldBehavior
                                                      std::vector<std::string>{},  // fields
                                                      std::move(fieldNames),       // projectFields
                                                      std::move(finalSlots),       // projectVars
                                                      true,                        // forceNewObject
                                                      false,  // returnOldObject
                                                      nodeId);

    return {std::move(outStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::makeUnionForTailableCollScan(const QuerySolutionNode* root,
                                                    const PlanStageReqs& reqs) {
    using namespace std::literals;
    invariant(!reqs.getIndexKeyBitset());

    // Register a SlotId in the global environment which would contain a recordId to resume a
    // tailable collection scan from. A PlanStage executor will track the last seen recordId and
    // will reset a SlotAccessor for the resumeRecordIdSlot with this recordId.
    auto resumeRecordIdSlot = _data.env->registerSlot(
        "resumeRecordId"_sd, sbe::value::TypeTags::Nothing, 0, false, &_slotIdGenerator);

    // For tailable collection scan we need to build a special union sub-tree consisting of two
    // branches:
    //   1) An anchor branch implementing an initial collection scan before the first EOF is hit.
    //   2) A resume branch implementing all consecutive collection scans from a recordId which was
    //      seen last.
    //
    // The 'makeStage' parameter is used to build a PlanStage tree which is served as a root stage
    // for each of the union branches. The same mechanism is used to build each union branch, and
    // the special logic which needs to be triggered depending on which branch we build is
    // controlled by setting the isTailableCollScanResumeBranch flag in PlanStageReqs.
    auto makeUnionBranch = [&](bool isTailableCollScanResumeBranch)
        -> std::pair<sbe::value::SlotVector, std::unique_ptr<sbe::PlanStage>> {
        auto childReqs = reqs;
        childReqs.setIsTailableCollScanResumeBranch(isTailableCollScanResumeBranch);
        auto [branch, outputs] = build(root, childReqs);

        auto branchSlots = sbe::makeSV();
        outputs.forEachSlot(reqs, [&](auto&& slot) { branchSlots.push_back(slot); });

        return {std::move(branchSlots), std::move(branch)};
    };

    // Build an anchor branch of the union and add a constant filter on top of it, so that it would
    // only execute on an initial collection scan, that is, when resumeRecordId is not available
    // yet.
    auto&& [anchorBranchSlots, anchorBranch] = makeUnionBranch(false);
    anchorBranch = sbe::makeS<sbe::FilterStage<true>>(
        std::move(anchorBranch),
        makeNot(makeFunction("exists"_sd, sbe::makeE<sbe::EVariable>(resumeRecordIdSlot))),
        root->nodeId());

    // Build a resume branch of the union and add a constant filter on op of it, so that it would
    // only execute when we resume a collection scan from the resumeRecordId.
    auto&& [resumeBranchSlots, resumeBranch] = makeUnionBranch(true);
    resumeBranch = sbe::makeS<sbe::FilterStage<true>>(
        sbe::makeS<sbe::LimitSkipStage>(std::move(resumeBranch), boost::none, 1, root->nodeId()),
        sbe::makeE<sbe::EFunction>("exists"_sd,
                                   sbe::makeEs(sbe::makeE<sbe::EVariable>(resumeRecordIdSlot))),
        root->nodeId());

    invariant(anchorBranchSlots.size() == resumeBranchSlots.size());

    // A vector of the output slots for each union branch.
    auto branchSlots = makeVector<sbe::value::SlotVector>(std::move(anchorBranchSlots),
                                                          std::move(resumeBranchSlots));

    auto unionOutputSlots = sbe::makeSV();

    PlanStageSlots outputs(reqs, &_slotIdGenerator);
    outputs.forEachSlot(reqs, [&](auto&& slot) { unionOutputSlots.push_back(slot); });

    // Branch output slots become the input slots to the union.
    auto unionStage =
        sbe::makeS<sbe::UnionStage>(sbe::makeSs(std::move(anchorBranch), std::move(resumeBranch)),
                                    branchSlots,
                                    unionOutputSlots,
                                    root->nodeId());

    return {std::move(unionStage), std::move(outputs)};
}

namespace {
/**
 * Given an SBE subtree 'childStage' which computes the shard key and puts it into the given
 * 'shardKeySlot', augments the SBE plan to actually perform shard filtering. Namely, a FilterStage
 * is added at the root of the tree whose filter expression uses 'shardFilterer' to determine
 * whether the shard key value in 'shardKeySlot' belongs to an owned range or not.
 */
auto buildShardFilterGivenShardKeySlot(sbe::value::SlotId shardKeySlot,
                                       std::unique_ptr<sbe::PlanStage> childStage,
                                       std::unique_ptr<ShardFilterer> shardFilterer,
                                       PlanNodeId nodeId) {
    auto shardFilterFn =
        makeFunction("shardFilter",
                     makeConstant(sbe::value::TypeTags::shardFilterer,
                                  sbe::value::bitcastFrom<ShardFilterer*>(shardFilterer.release())),
                     sbe::makeE<sbe::EVariable>(shardKeySlot));

    return sbe::makeS<sbe::FilterStage<false>>(
        std::move(childStage), std::move(shardFilterFn), nodeId);
}
}  // namespace

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildShardFilterCovered(const ShardingFilterNode* filterNode,
                                               std::unique_ptr<ShardFilterer> shardFilterer,
                                               BSONObj shardKeyPattern,
                                               BSONObj indexKeyPattern,
                                               const QuerySolutionNode* child,
                                               PlanStageReqs childReqs) {
    StringDataSet shardKeyFields;
    for (auto&& shardKeyElt : shardKeyPattern) {
        shardKeyFields.insert(shardKeyElt.fieldNameStringData());
    }

    // Save the bit vector describing the fields from the index that our parent requires. The shard
    // filtering process may require additional fields that are not needed by the parent (for
    // example, if the parent is projecting field "a" but the shard key is {a: 1, b: 1}). We will
    // need the parent's reqs later on so that we can hand the correct slot vector for these fields
    // back to our parent.
    auto parentIndexKeyReqs = childReqs.getIndexKeyBitset();

    // Determine the set of fields from the index required to obtain the shard key and union those
    // with the set of fields from the index required by the parent stage.
    auto [shardKeyIndexReqs, _] = makeIndexKeyInclusionSet(indexKeyPattern, shardKeyFields);
    const auto ixKeyBitset =
        parentIndexKeyReqs.value_or(sbe::IndexKeysInclusionSet{}) | shardKeyIndexReqs;
    childReqs.getIndexKeyBitset() = ixKeyBitset;

    auto [stage, outputs] = build(child, childReqs);
    tassert(5562302, "Expected child to produce index key slots", outputs.getIndexKeySlots());

    // Maps from key name -> (index in outputs.getIndexKeySlots(), is hashed).
    auto ixKeyPatternFieldToSlotIdx = [&, childOutputs = std::ref(outputs)]() {
        StringDataMap<std::pair<sbe::value::SlotId, bool>> ret;

        // Keeps track of which component we're reading in the index key pattern.
        size_t i = 0;
        // Keeps track of the index we are in the slot vector produced by the ix scan. The slot
        // vector produced by the ix scan may be a subset of the key pattern.
        size_t slotIdx = 0;
        for (auto&& ixPatternElt : indexKeyPattern) {
            if (shardKeyFields.count(ixPatternElt.fieldNameStringData())) {
                const bool isHashed = ixPatternElt.valueStringData() == IndexNames::HASHED;
                const auto slotId = (*childOutputs.get().getIndexKeySlots())[slotIdx];
                ret.emplace(ixPatternElt.fieldNameStringData(), std::make_pair(slotId, isHashed));
            }

            if (ixKeyBitset[i]) {
                ++slotIdx;
            }
            ++i;
        }
        return ret;
    }();

    // Build a project stage to deal with hashed shard keys. This step *could* be skipped if we're
    // dealing with non-hashed sharding, but it's done this way for sake of simplicity.
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projections;
    sbe::value::SlotVector fieldSlots;
    std::vector<std::string> projectFields;
    for (auto&& shardKeyPatternElt : shardKeyPattern) {
        auto it = ixKeyPatternFieldToSlotIdx.find(shardKeyPatternElt.fieldNameStringData());
        tassert(5562303, "Could not find element", it != ixKeyPatternFieldToSlotIdx.end());
        auto [slotId, ixKeyEltHashed] = it->second;

        // Get the value stored in the index for this component of the shard key. We may have to
        // hash it.
        auto elem = makeVariable(slotId);

        const bool shardKeyEltHashed = ShardKeyPattern::isHashedPatternEl(shardKeyPatternElt);

        if (shardKeyEltHashed) {
            if (ixKeyEltHashed) {
                // (1) The index stores hashed data and the shard key field is hashed.
                // Nothing to do here. We can apply shard filtering with no other changes.
            } else {
                // (2) The shard key field is hashed but the index stores unhashed data. We must
                // apply the hash function before passing this off to the shard filter.
                elem = makeFunction("shardHash"_sd, std::move(elem));
            }
        } else {
            if (ixKeyEltHashed) {
                // (3) The index stores hashed data but the shard key is not hashed. This is a bug.
                MONGO_UNREACHABLE_TASSERT(5562300);
            } else {
                // (4) The shard key field is not hashed, and the index does not store hashed data.
                // Again, we do nothing here.
            }
        }

        fieldSlots.push_back(_slotIdGenerator.generate());
        projectFields.push_back(shardKeyPatternElt.fieldName());
        projections.emplace(fieldSlots.back(), std::move(elem));
    }

    auto projectStage = sbe::makeS<sbe::ProjectStage>(
        std::move(stage), std::move(projections), filterNode->nodeId());

    auto shardKeySlot = _slotIdGenerator.generate();
    auto mkObjStage = sbe::makeS<sbe::MakeBsonObjStage>(std::move(projectStage),
                                                        shardKeySlot,
                                                        boost::none,
                                                        boost::none,
                                                        std::vector<std::string>{},
                                                        std::move(projectFields),
                                                        fieldSlots,
                                                        true,
                                                        false,
                                                        filterNode->nodeId());

    auto filterStage = buildShardFilterGivenShardKeySlot(
        shardKeySlot, std::move(mkObjStage), std::move(shardFilterer), filterNode->nodeId());

    outputs.setIndexKeySlots(!parentIndexKeyReqs ? boost::none
                                                 : boost::optional<sbe::value::SlotVector>{
                                                       makeIndexKeyOutputSlotsMatchingParentReqs(
                                                           indexKeyPattern,
                                                           *parentIndexKeyReqs,
                                                           *childReqs.getIndexKeyBitset(),
                                                           *outputs.getIndexKeySlots())});

    return {std::move(filterStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildShardFilter(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto filterNode = static_cast<const ShardingFilterNode*>(root);

    // If we're sharded make sure that we don't return data that isn't owned by the shard. This
    // situation can occur when pending documents from in-progress migrations are inserted and when
    // there are orphaned documents from aborted migrations. To check if the document is owned by
    // the shard, we need to own a 'ShardFilterer', and extract the document's shard key as a
    // BSONObj.
    auto shardFilterer = _shardFiltererFactory->makeShardFilterer(_opCtx);
    auto shardKeyPattern = shardFilterer->getKeyPattern().toBSON();

    // Determine if our child is an index scan and extract it's key pattern, or empty BSONObj if our
    // child is not an IXSCAN node.
    BSONObj indexKeyPattern = [&]() {
        auto childNode = filterNode->children[0];
        switch (childNode->getType()) {
            case StageType::STAGE_IXSCAN:
                return static_cast<const IndexScanNode*>(childNode)->index.keyPattern;
            case StageType::STAGE_VIRTUAL_SCAN:
                return static_cast<const VirtualScanNode*>(childNode)->indexKeyPattern;
            default:
                return BSONObj{};
        }
    }();

    // If we're not required to fill out the 'kResult' slot, then instead we can request a slot from
    // the child for each of the fields which constitute the shard key. This allows us to avoid
    // materializing an intermediate object for plans where shard filtering can be performed based
    // on the contents of index keys.
    //
    // We only apply this optimization in the special case that the child QSN is an IXSCAN, since in
    // this case we can request exactly the fields we need according to their position in the index
    // key pattern.
    auto childReqs = reqs.copy().setIf(kResult, indexKeyPattern.isEmpty());
    if (!childReqs.has(kResult)) {
        return buildShardFilterCovered(filterNode,
                                       std::move(shardFilterer),
                                       std::move(shardKeyPattern),
                                       std::move(indexKeyPattern),
                                       filterNode->children[0],
                                       std::move(childReqs));
    }

    auto [stage, outputs] = build(filterNode->children[0], childReqs);

    // Build an expression to extract the shard key from the document based on the shard key
    // pattern. To do this, we iterate over the shard key pattern parts and build nested 'getField'
    // expressions. This will handle single-element paths, and dotted paths for each shard key part.
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projections;
    sbe::value::SlotVector fieldSlots;
    std::vector<std::string> projectFields;
    std::unique_ptr<sbe::EExpression> bindShardKeyPart;

    for (auto&& keyPatternElem : shardKeyPattern) {
        auto fieldRef = FieldRef{keyPatternElem.fieldNameStringData()};
        fieldSlots.push_back(_slotIdGenerator.generate());
        projectFields.push_back(fieldRef.dottedField().toString());

        auto currentFieldSlot = sbe::makeE<sbe::EVariable>(outputs.get(kResult));
        auto shardKeyBinding =
            generateShardKeyBinding(fieldRef, _frameIdGenerator, std::move(currentFieldSlot), 0);

        // If this is a hashed shard key then compute the hash value.
        if (ShardKeyPattern::isHashedPatternEl(keyPatternElem)) {
            shardKeyBinding = makeFunction("shardHash"_sd, std::move(shardKeyBinding));
        }

        projections.emplace(fieldSlots.back(), std::move(shardKeyBinding));
    }

    auto shardKeySlot{_slotIdGenerator.generate()};

    // Build an object which will hold a flattened shard key from the projections above.
    auto shardKeyObjStage = sbe::makeS<sbe::MakeBsonObjStage>(
        sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projections), root->nodeId()),
        shardKeySlot,
        boost::none,
        boost::none,
        std::vector<std::string>{},
        projectFields,
        fieldSlots,
        true,
        false,
        root->nodeId());

    // Build a project stage that checks if any of the fieldSlots for the shard key parts are an
    // Array which is represented by Nothing.
    invariant(fieldSlots.size() > 0);
    auto arrayChecks = makeNot(sbe::makeE<sbe::EFunction>(
        "exists", sbe::makeEs(sbe::makeE<sbe::EVariable>(fieldSlots[0]))));
    for (size_t ind = 1; ind < fieldSlots.size(); ++ind) {
        arrayChecks = makeBinaryOp(
            sbe::EPrimBinary::Op::logicOr,
            std::move(arrayChecks),
            makeNot(makeFunction("exists", sbe::makeE<sbe::EVariable>(fieldSlots[ind]))));
    }
    arrayChecks = sbe::makeE<sbe::EIf>(std::move(arrayChecks),
                                       sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0),
                                       sbe::makeE<sbe::EVariable>(shardKeySlot));

    auto finalShardKeySlot{_slotIdGenerator.generate()};

    auto finalShardKeyObjStage = makeProjectStage(
        std::move(shardKeyObjStage), root->nodeId(), finalShardKeySlot, std::move(arrayChecks));

    return {buildShardFilterGivenShardKeySlot(finalShardKeySlot,
                                              std::move(finalShardKeyObjStage),
                                              std::move(shardFilterer),
                                              root->nodeId()),
            std::move(outputs)};
}

// Returns a non-null pointer to the root of a plan tree, or a non-OK status if the PlanStage tree
// could not be constructed.
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::build(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    static const stdx::unordered_map<
        StageType,
        std::function<std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>(
            SlotBasedStageBuilder&, const QuerySolutionNode* root, const PlanStageReqs& reqs)>>
        kStageBuilders = {
            {STAGE_COLLSCAN, &SlotBasedStageBuilder::buildCollScan},
            {STAGE_VIRTUAL_SCAN, &SlotBasedStageBuilder::buildVirtualScan},
            {STAGE_IXSCAN, &SlotBasedStageBuilder::buildIndexScan},
            {STAGE_FETCH, &SlotBasedStageBuilder::buildFetch},
            {STAGE_LIMIT, &SlotBasedStageBuilder::buildLimit},
            {STAGE_SKIP, &SlotBasedStageBuilder::buildSkip},
            {STAGE_SORT_SIMPLE, &SlotBasedStageBuilder::buildSort},
            {STAGE_SORT_DEFAULT, &SlotBasedStageBuilder::buildSort},
            {STAGE_SORT_KEY_GENERATOR, &SlotBasedStageBuilder::buildSortKeyGeneraror},
            {STAGE_PROJECTION_SIMPLE, &SlotBasedStageBuilder::buildProjectionSimple},
            {STAGE_PROJECTION_DEFAULT, &SlotBasedStageBuilder::buildProjectionDefault},
            {STAGE_PROJECTION_COVERED, &SlotBasedStageBuilder::buildProjectionCovered},
            {STAGE_OR, &SlotBasedStageBuilder::buildOr},
            // In SBE TEXT_OR behaves like a regular OR. All the work to support "textScore"
            // metadata is done outside of TEXT_OR, unlike the legacy implementation.
            {STAGE_TEXT_OR, &SlotBasedStageBuilder::buildOr},
            {STAGE_TEXT_MATCH, &SlotBasedStageBuilder::buildTextMatch},
            {STAGE_RETURN_KEY, &SlotBasedStageBuilder::buildReturnKey},
            {STAGE_EOF, &SlotBasedStageBuilder::buildEof},
            {STAGE_AND_HASH, &SlotBasedStageBuilder::buildAndHash},
            {STAGE_AND_SORTED, &SlotBasedStageBuilder::buildAndSorted},
            {STAGE_SORT_MERGE, &SlotBasedStageBuilder::buildSortMerge},
            {STAGE_GROUP, &SlotBasedStageBuilder::buildGroup},
            {STAGE_SHARDING_FILTER, &SlotBasedStageBuilder::buildShardFilter}};

    tassert(4822884,
            str::stream() << "Unsupported QSN in SBE stage builder: " << root->toString(),
            kStageBuilders.find(root->getType()) != kStageBuilders.end());

    // If this plan is for a tailable cursor scan, and we're not already in the process of building
    // a special union sub-tree implementing such scans, then start building a union sub-tree. Note
    // that LIMIT or SKIP stage is used as a splitting point of the two union branches, if present,
    // because we need to apply limit (or skip) only in the initial scan (in the anchor branch), and
    // the resume branch should not have it.
    switch (root->getType()) {
        case STAGE_COLLSCAN:
        case STAGE_LIMIT:
        case STAGE_SKIP:
            if (_cq.getFindCommandRequest().getTailable() &&
                !reqs.getIsBuildingUnionForTailableCollScan()) {
                auto childReqs = reqs;
                childReqs.setIsBuildingUnionForTailableCollScan(true);
                return makeUnionForTailableCollScan(root, childReqs);
            }
        default:
            break;
    }

    return std::invoke(kStageBuilders.at(root->getType()), *this, root, reqs);
}
}  // namespace mongo::stage_builder
