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

#pragma once

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/hash/hash.h>
#include <absl/meta/type_traits.h>
#include <absl/strings/string_view.h>
#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/collection_helpers.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/trial_period_utils.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/sbe_stage_builder_plan_data.h"
#include "mongo/db/query/sbe_stage_builder_type_signature.h"
#include "mongo/db/query/shard_filterer_factory_interface.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::stage_builder {

class PlanStageReqs;
class PlanStageSlots;

struct PlanStageData;
sbe::value::SlotVector getSlotsToForward(const PlanStageReqs& reqs,
                                         const PlanStageSlots& outputs,
                                         const sbe::value::SlotVector& exclude = sbe::makeSV());

/**
 * This function prepares the SBE tree for execution, such as attaching the OperationContext,
 * ensuring that the SBE tree is registered with the PlanYieldPolicySBE and populating the
 * "RuntimeEnvironment".
 *
 * The caller should pass true for 'preparingFromCache' if the SBE plan being prepared is being
 * recovered from the SBE plan cache.
 */
void prepareSlotBasedExecutableTree(OperationContext* opCtx,
                                    sbe::PlanStage* root,
                                    PlanStageData* data,
                                    const CanonicalQuery& cq,
                                    const MultipleCollectionAccessor& collections,
                                    PlanYieldPolicySBE* yieldPolicy,
                                    bool preparingFromCache);

/**
 * Associate a slot with a signature representing all the possible types that the value stored at
 * runtime in the slot can assume.
 */
struct TypedSlot {
    sbe::value::SlotId slotId;
    TypeSignature typeSignature;
};

/**
 * The PlanStageSlots class is used by SlotBasedStageBuilder to return the output slots produced
 * after building a stage.
 */
class PlanStageSlots {
public:
    // The _slots map is capable of holding different "classes" of slots:
    // 1) kMeta slots are used to hold the current document (kResult), record ID (kRecordId), and
    //    various pieces of metadata.
    // 2) kField slots represent the values of top-level fields, or in some cases of dotted field
    //    paths (when we are getting the dotted field from a non-multikey index and we know no array
    //    traversal is needed). These slots hold the actual values of the fields / field paths (not
    //    the sort key or collation comparison key for the field).
    // 3) kSortKey slots represent the raw key value that comes from an ixscan / ixseek stage for a
    //    given field path. This raw key value can be used for sorting / comparison, but it is not
    //    always equal to the actual value of the field path (for example, if the key is coming from
    //    an index that has a non-simple collation).
    // 4) kPathExpr slots represent the value obtained from evaluating an 'ExpressionFieldPath'.
    //    Typically, this is requested by stages that wish to avoid generating duplicate
    //    expressions for path traversal (for example, $group stages which reference the same
    //    field path across multiple accumulators).
    enum class Type {
        kMeta,
        kField,
        kSortKey,
        kPathExpr,
    };

    using Name = std::pair<Type, StringData>;
    using OwnedName = std::pair<Type, std::string>;

    static constexpr auto kMeta = Type::kMeta;
    static constexpr auto kField = Type::kField;
    static constexpr auto kSortKey = Type::kSortKey;
    static constexpr auto kPathExpr = Type::kPathExpr;

    static constexpr Name kResult = {kMeta, "result"_sd};
    static constexpr Name kRecordId = {kMeta, "recordId"_sd};
    static constexpr Name kReturnKey = {kMeta, "returnKey"_sd};
    static constexpr Name kSnapshotId = {kMeta, "snapshotId"_sd};
    static constexpr Name kIndexIdent = {kMeta, "indexIdent"_sd};
    static constexpr Name kIndexKey = {kMeta, "indexKey"_sd};
    static constexpr Name kIndexKeyPattern = {kMeta, "indexKeyPattern"_sd};
    static constexpr Name kMetadataSearchScore = {kMeta, "metadataSearchScore"_sd};
    static constexpr Name kMetadataSearchHighlights = {kMeta, "metadataSearchHighlights"_sd};
    static constexpr Name kMetadataSearchDetails = {kMeta, "metadataSearchDetails"_sd};
    static constexpr Name kMetadataSearchSortValues = {kMeta, "metadataSearchSortValues"_sd};

    PlanStageSlots() = default;

    PlanStageSlots(const PlanStageReqs& reqs, sbe::value::SlotIdGenerator* slotIdGenerator);

    bool has(const Name& str) const {
        return _slots.count(str);
    }

    TypedSlot get(const Name& str) const {
        auto it = _slots.find(str);
        invariant(it != _slots.end());
        return it->second;
    }

    boost::optional<TypedSlot> getIfExists(const Name& str) const {
        if (auto it = _slots.find(str); it != _slots.end()) {
            return it->second;
        }
        return boost::none;
    }

    boost::optional<sbe::value::SlotId> getSlotIfExists(const Name& str) const {
        if (auto it = _slots.find(str); it != _slots.end()) {
            return it->second.slotId;
        }
        return boost::none;
    }

    void set(const Name& str, sbe::value::SlotId slot) {
        set(str, TypedSlot{slot, TypeSignature::kAnyScalarType});
    }

    void set(OwnedName str, sbe::value::SlotId slot) {
        set(std::move(str), TypedSlot{slot, TypeSignature::kAnyScalarType});
    }

    void set(const Name& str, TypedSlot slot) {
        _slots.insert_or_assign(str, slot);
    }

    void set(OwnedName str, TypedSlot slot) {
        _slots.insert_or_assign(std::move(str), slot);
    }

    void clear(const Name& str) {
        _slots.erase(str);
    }

    void clearAllFields() {
        for (auto it = _slots.begin(); it != _slots.end();) {
            if (it->first.first == kField) {
                _slots.erase(it++);
                continue;
            }
            ++it;
        }
    }

    /**
     * This method applies an action to some/all of the slots within this struct. For each slot in
     * this struct, the action is will be applied to the slot if (and only if) the corresponding
     * flag in 'reqs' is true.
     */
    inline void forEachSlot(const PlanStageReqs& reqs,
                            const std::function<void(const TypedSlot&)>& fn) const;

    inline void forEachSlot(const PlanStageReqs& reqs,
                            const std::function<void(const TypedSlot&, const Name&)>& fn) const;
    inline void forEachSlot(const std::function<void(const TypedSlot&)>& fn) const;
    inline void forEachSlot(const std::function<void(const Name&, const TypedSlot&)>& fn) const;
    inline void clearNonRequiredSlots(const PlanStageReqs& reqs);

    struct NameHasher {
        using is_transparent = void;
        size_t operator()(const Name& p) const noexcept {
            auto h{std::pair{p.first, absl::string_view{p.second.rawData(), p.second.size()}}};
            return absl::Hash<decltype(h)>{}(h);
        }
    };

    struct NameEq : std::equal_to<Name> {
        using is_transparent = void;
    };

    template <typename V>
    using NameMap = absl::flat_hash_map<OwnedName, V, NameHasher, NameEq>;
    using NameSet = absl::flat_hash_set<OwnedName, NameHasher, NameEq>;

private:
    NameMap<TypedSlot> _slots;
};

/**
 * The PlanStageReqs class is used by SlotBasedStageBuilder to represent the incoming requirements
 * and context when building a stage.
 */
class PlanStageReqs {
public:
    using Type = PlanStageSlots::Type;
    using Name = PlanStageSlots::Name;
    using OwnedName = PlanStageSlots::OwnedName;

    static constexpr auto kMeta = PlanStageSlots::Type::kMeta;
    static constexpr auto kField = PlanStageSlots::Type::kField;
    static constexpr auto kSortKey = PlanStageSlots::Type::kSortKey;

    PlanStageReqs copy() const {
        return *this;
    }

    bool has(const Name& str) const {
        return _slots.contains(str);
    }

    PlanStageReqs& set(const Name& str) {
        _slots.emplace(str);
        return *this;
    }

    PlanStageReqs& set(OwnedName str) {
        _slots.emplace(std::move(str));
        return *this;
    }

    PlanStageReqs& set(const std::vector<Name>& strs) {
        _slots.insert(strs.begin(), strs.end());
        return *this;
    }

    PlanStageReqs& set(std::vector<OwnedName> strs) {
        _slots.insert(std::make_move_iterator(strs.begin()), std::make_move_iterator(strs.end()));
        return *this;
    }

    PlanStageReqs& setIf(const Name& str, bool condition) {
        if (condition) {
            _slots.emplace(str);
        }
        return *this;
    }

    PlanStageReqs& setFields(std::vector<std::string> strs) {
        for (size_t i = 0; i < strs.size(); ++i) {
            _slots.emplace(kField, std::move(strs[i]));
        }
        return *this;
    }

    PlanStageReqs& setSortKeys(std::vector<std::string> strs) {
        for (size_t i = 0; i < strs.size(); ++i) {
            _slots.emplace(kSortKey, std::move(strs[i]));
        }
        return *this;
    }

    PlanStageReqs& clear(const Name& str) {
        _slots.erase(str);
        return *this;
    }

    bool getIsBuildingUnionForTailableCollScan() const {
        return _isBuildingUnionForTailableCollScan;
    }

    void setIsBuildingUnionForTailableCollScan(bool b) {
        _isBuildingUnionForTailableCollScan = b;
    }

    bool getIsTailableCollScanResumeBranch() const {
        return _isTailableCollScanResumeBranch;
    }

    void setIsTailableCollScanResumeBranch(bool b) {
        _isTailableCollScanResumeBranch = b;
    }

    void setTargetNamespace(const NamespaceString& nss) {
        _targetNamespace = nss;
    }

    const NamespaceString& getTargetNamespace() const {
        return _targetNamespace;
    }

    bool hasType(Type t) const {
        return std::any_of(
            _slots.begin(), _slots.end(), [t](auto& item) { return item.first == t; });
    }
    bool hasFields() const {
        return hasType(kField);
    }
    bool hasSortKeys() const {
        return hasType(kSortKey);
    }

    std::vector<std::string> getOfType(Type t) const {
        std::vector<std::string> res;
        for (const auto& [type, str] : _slots) {
            if (type == t) {
                res.push_back(str);
            }
        }
        std::sort(res.begin(), res.end());
        return res;
    }
    std::vector<std::string> getFields() const {
        return getOfType(kField);
    }
    std::vector<std::string> getSortKeys() const {
        return getOfType(kSortKey);
    }

    PlanStageReqs& clearAllOfType(Type t) {
        absl::erase_if(_slots, [t](auto& item) { return item.first == t; });
        return *this;
    }
    PlanStageReqs& clearAllFields() {
        return clearAllOfType(kField);
    }
    PlanStageReqs& clearAllSortKeys() {
        return clearAllOfType(kSortKey);
    }

    friend PlanStageSlots::PlanStageSlots(const PlanStageReqs& reqs,
                                          sbe::value::SlotIdGenerator* slotIdGenerator);

    inline void forEachReq(const std::function<void(const Name&)>& fn) const {
        for (const auto& name : _slots) {
            fn(name);
        }
    }

    friend void PlanStageSlots::forEachSlot(const PlanStageReqs& reqs,
                                            const std::function<void(const TypedSlot&)>& fn) const;

    friend void PlanStageSlots::forEachSlot(
        const PlanStageReqs& reqs,
        const std::function<void(const TypedSlot&, const Name&)>& fn) const;

private:
    PlanStageSlots::NameSet _slots;

    // When we're in the middle of building a special union sub-tree implementing a tailable cursor
    // collection scan, this flag will be set to true. Otherwise this flag will be false.
    bool _isBuildingUnionForTailableCollScan{false};

    // When we're in the middle of building a special union sub-tree implementing a tailable cursor
    // collection scan, this flag indicates whether we're currently building an anchor or resume
    // branch. At all other times, this flag will be false.
    bool _isTailableCollScanResumeBranch{false};

    // Tracks the current namespace that we're building a plan over. Given that the stage builder
    // can build plans for multiple namespaces, a node in the tree that targets a namespace
    // different from its parent node can set this value to notify any child nodes of the correct
    // namespace.
    NamespaceString _targetNamespace;
};

void PlanStageSlots::forEachSlot(const PlanStageReqs& reqs,
                                 const std::function<void(const TypedSlot&)>& fn) const {
    for (const auto& name : reqs._slots) {
        auto it = _slots.find(name);
        tassert(7050900,
                str::stream() << "Could not find " << static_cast<int>(name.first) << ":'"
                              << name.second << "' in the slot map, expected slot to exist",
                it != _slots.end());

        fn(it->second);
    }
}

void PlanStageSlots::forEachSlot(
    const PlanStageReqs& reqs, const std::function<void(const TypedSlot&, const Name&)>& fn) const {
    for (const auto& name : reqs._slots) {
        auto it = _slots.find(name);
        tassert(7050901,
                str::stream() << "Could not find " << static_cast<int>(name.first) << ":'"
                              << name.second << "' in the slot map, expected slot to exist",
                it != _slots.end());
        fn(it->second, name);
    }
}

void PlanStageSlots::forEachSlot(const std::function<void(const TypedSlot&)>& fn) const {
    for (const auto& entry : _slots) {
        fn(entry.second);
    }
}

void PlanStageSlots::forEachSlot(
    const std::function<void(const Name&, const TypedSlot&)>& fn) const {
    for (const auto& entry : _slots) {
        fn(entry.first, entry.second);
    }
}

void PlanStageSlots::clearNonRequiredSlots(const PlanStageReqs& reqs) {
    auto it = _slots.begin();
    while (it != _slots.end()) {
        auto& name = it->first;
        // We never clear kResult, regardless of whether it is required by 'reqs'.
        if (_slots.key_eq()(name, kResult) || reqs.has(name)) {
            ++it;
        } else {
            _slots.erase(it++);
        }
    }
}

/**
 * A stage builder which builds an executable tree using slot-based PlanStages.
 */
class SlotBasedStageBuilder final
    : public StageBuilder<std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageData>> {
public:
    using PlanType = std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageData>;
    using BaseType = StageBuilder<PlanType>;

    static constexpr auto kResult = PlanStageSlots::kResult;
    static constexpr auto kRecordId = PlanStageSlots::kRecordId;
    static constexpr auto kReturnKey = PlanStageSlots::kReturnKey;
    static constexpr auto kSnapshotId = PlanStageSlots::kSnapshotId;
    static constexpr auto kIndexIdent = PlanStageSlots::kIndexIdent;
    static constexpr auto kIndexKey = PlanStageSlots::kIndexKey;
    static constexpr auto kIndexKeyPattern = PlanStageSlots::kIndexKeyPattern;
    static constexpr auto kMetadataSearchScore = PlanStageSlots::kMetadataSearchScore;
    static constexpr auto kMetadataSearchHighlights = PlanStageSlots::kMetadataSearchHighlights;
    static constexpr auto kMetadataSearchDetails = PlanStageSlots::kMetadataSearchDetails;
    static constexpr auto kMetadataSearchSortValues = PlanStageSlots::kMetadataSearchSortValues;

    SlotBasedStageBuilder(OperationContext* opCtx,
                          const MultipleCollectionAccessor& collections,
                          const CanonicalQuery& cq,
                          const QuerySolution& solution,
                          PlanYieldPolicySBE* yieldPolicy);

    /**
     * This method will build an SBE PlanStage tree for QuerySolutionNode 'root' and its
     * descendents.
     *
     * This method is a wrapper around 'build(const QuerySolutionNode*, const PlanStageReqs&)'.
     */
    PlanType build(const QuerySolutionNode* root) final;

private:
    /**
     * This method will build an SBE PlanStage tree for QuerySolutionNode 'root' and its
     * descendents.
     *
     * Based on the type of 'root', this method will dispatch to the appropriate buildXXX() method.
     * This method will also handle generating calls to getField() to satisfy kField reqs that were
     * not satisfied by the buildXXX() method.
     */
    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> build(const QuerySolutionNode* node,
                                                                     const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildCollScan(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildVirtualScan(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildIndexScan(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildCountScan(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildColumnScan(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildFetch(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildLimit(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildSkip(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildSort(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildSortCovered(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildSortKeyGenerator(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildSortMerge(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildMatch(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildReplaceRoot(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildProjectionSimple(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildProjectionCovered(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildProjectionDefault(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildProjectionDefaultCovered(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildOr(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildTextMatch(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildReturnKey(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildEof(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildAndHash(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildAndSorted(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> makeUnionForTailableCollScan(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildShardFilter(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildSearch(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildWindow(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    /**
     * Constructs an optimized SBE plan for 'root' in the case that the fields of the shard key
     * pattern are provided by the child index scan. In this case, the SBE plan for the child
     * index scan node will fill out slots for the necessary components of the index key. These
     * slots can be read directly in order to determine the shard key that should be passed to the
     * 'shardFiltererSlot'.
     */
    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildShardFilterCovered(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildGroup(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildLookup(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildUnpackTsBucket(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    /**
     * Returns a CollectionPtr corresponding to the collection that we are currently building a
     * plan over. If no current namespace is configured, a CollectionPtr referencing the main
     * collection tracked by '_collections' is returned.
     */
    const CollectionPtr& getCurrentCollection(const PlanStageReqs& reqs) const;

    sbe::value::SlotIdGenerator _slotIdGenerator;
    sbe::value::FrameIdGenerator _frameIdGenerator;
    sbe::value::SpoolIdGenerator _spoolIdGenerator;

    // Hash set tracking the InListDatas used by the SBE plan being built.
    absl::flat_hash_set<InListData*> _inListsSet;

    // Hash set tracking the Collators used by the SBE plan being built.
    absl::flat_hash_map<const CollatorInterface*, const CollatorInterface*> _collatorMap;

    const MultipleCollectionAccessor& _collections;

    // Indicates the main namespace that we're building a plan over.
    NamespaceString _mainNss;

    PlanYieldPolicySBE* const _yieldPolicy{nullptr};

    // Aside from generating the PlanStage tree, this builder also produces a few auxiliary data
    // structures that are needed to execute the tree: the RuntimeEnvironment, the CompileCtx,
    // and the PlanStageStaticData. Note that the PlanStageStaticData ('_data') is mutable inside
    // SlotBasedStageBuilder, but after the 'build(const QuerySolutionNode*)' method is called the
    // data will become immutable.
    Environment _env;
    std::unique_ptr<PlanStageStaticData> _data;

    bool _buildHasStarted{false};

    // Common parameters to SBE stage builder functions.
    StageBuilderState _state;
};
}  // namespace mongo::stage_builder
