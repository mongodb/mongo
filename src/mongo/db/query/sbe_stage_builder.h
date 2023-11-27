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

/**
 * Returns a vector of the slot IDs corresponding to 'reqs', ordered by slot name. This function
 * is intended for use in situations where a branch or union is being constructed and the contents
 * of multiple PlanStageSlots objects need to be merged together.
 *
 * Note that a given slot ID may appear more than once in the SlotVector returned. This is
 * the intended behavior.
 */
sbe::value::SlotVector getSlotsOrderedByName(const PlanStageReqs& reqs,
                                             const PlanStageSlots& outputs);

/**
 * Returns a vector of the unique slot IDs needed by 'reqs', ordered by slot ID. This function is
 * intended for use in situations where a join or sort or something else is being constructed and
 * a PlanStageSlot's contents need to be "forwarded" through a PlanStage.
 */
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
                                    bool preparingFromCache,
                                    RemoteCursorMap* remoteCursors = nullptr);

std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData>
buildSearchMetadataExecutorSBE(OperationContext* opCtx,
                               const CanonicalQuery& cq,
                               size_t remoteCursorId,
                               RemoteCursorMap* remoteCursors,
                               PlanYieldPolicySBE* yieldPolicy);

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
    // The _slotNameToIdMap map is capable of holding different "types" of slots:
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
    // 5) kFilterCellField slots represent the value obtained from evaluating a dotted path on top
    //    of a timeseries bucket, expanding arrays as they are encountered during the traversal.
    enum class SlotType {
        kMeta,
        kField,
        kSortKey,
        kPathExpr,
        kFilterCellField,
    };

    // Slot "names" in this file are really type-and-name pairs.
    using UnownedSlotName = std::pair<SlotType, StringData>;
    using OwnedSlotName = std::pair<SlotType, std::string>;

    struct NameHasher {
        using is_transparent = void;
        size_t operator()(const UnownedSlotName& p) const noexcept {
            auto h{std::pair{p.first, absl::string_view{p.second.rawData(), p.second.size()}}};
            return absl::Hash<decltype(h)>{}(h);
        }
    };

    struct NameEq : std::equal_to<UnownedSlotName> {
        using is_transparent = void;
    };

    using SlotNameMap = absl::flat_hash_map<OwnedSlotName, TypedSlot, NameHasher, NameEq>;
    using SlotNameSet = absl::flat_hash_set<OwnedSlotName, NameHasher, NameEq>;

    static constexpr SlotType kMeta = SlotType::kMeta;
    static constexpr SlotType kField = SlotType::kField;
    static constexpr SlotType kSortKey = SlotType::kSortKey;
    static constexpr SlotType kPathExpr = SlotType::kPathExpr;
    static constexpr SlotType kFilterCellField = SlotType::kFilterCellField;

    static constexpr UnownedSlotName kResult = {kMeta, "result"_sd};
    static constexpr UnownedSlotName kRecordId = {kMeta, "recordId"_sd};
    static constexpr UnownedSlotName kReturnKey = {kMeta, "returnKey"_sd};
    static constexpr UnownedSlotName kSnapshotId = {kMeta, "snapshotId"_sd};
    static constexpr UnownedSlotName kIndexIdent = {kMeta, "indexIdent"_sd};
    static constexpr UnownedSlotName kIndexKey = {kMeta, "indexKey"_sd};
    static constexpr UnownedSlotName kIndexKeyPattern = {kMeta, "indexKeyPattern"_sd};
    static constexpr UnownedSlotName kMetadataSearchScore = {kMeta, "metadataSearchScore"_sd};
    static constexpr UnownedSlotName kMetadataSearchHighlights = {kMeta,
                                                                  "metadataSearchHighlights"_sd};
    static constexpr UnownedSlotName kMetadataSearchDetails = {kMeta, "metadataSearchDetails"_sd};
    static constexpr UnownedSlotName kMetadataSearchSortValues = {kMeta,
                                                                  "metadataSearchSortValues"_sd};
    static constexpr UnownedSlotName kMetadataSearchSequenceToken = {
        kMeta, "metadataSearchSequenceToken"_sd};

    PlanStageSlots() = default;

    PlanStageSlots(const PlanStageReqs& reqs, sbe::value::SlotIdGenerator* slotIdGenerator);

    bool has(const UnownedSlotName& str) const {
        return _slotNameToIdMap.count(str);
    }

    TypedSlot get(const UnownedSlotName& str) const {
        auto it = _slotNameToIdMap.find(str);
        invariant(it != _slotNameToIdMap.end());
        return it->second;
    }

    boost::optional<TypedSlot> getIfExists(const UnownedSlotName& str) const {
        if (auto it = _slotNameToIdMap.find(str); it != _slotNameToIdMap.end()) {
            return it->second;
        }
        return boost::none;
    }

    boost::optional<sbe::value::SlotId> getSlotIfExists(const UnownedSlotName& str) const {
        if (auto it = _slotNameToIdMap.find(str); it != _slotNameToIdMap.end()) {
            return it->second.slotId;
        }
        return boost::none;
    }

    void set(const UnownedSlotName& str, sbe::value::SlotId slot) {
        set(str, TypedSlot{slot, TypeSignature::kAnyScalarType});
    }

    void set(OwnedSlotName str, sbe::value::SlotId slot) {
        set(std::move(str), TypedSlot{slot, TypeSignature::kAnyScalarType});
    }

    void set(const UnownedSlotName& str, TypedSlot slot) {
        _slotNameToIdMap.insert_or_assign(str, slot);
    }

    void set(OwnedSlotName str, TypedSlot slot) {
        _slotNameToIdMap.insert_or_assign(std::move(str), slot);
    }

    void clear(const UnownedSlotName& str) {
        _slotNameToIdMap.erase(str);
    }

    // Clear a single field (SlotType::kField) in '_slotNameToIdMap' by its string name.
    void clearField(StringData fieldName) {
        auto it = _slotNameToIdMap.find(UnownedSlotName{kField, fieldName});
        if (it != _slotNameToIdMap.end()) {
            _slotNameToIdMap.erase(it);
        }
    }

    // Clear all fields (SlotType::kField) in '_slotNameToIdMap'.
    void clearAllFields() {
        for (auto it = _slotNameToIdMap.begin(); it != _slotNameToIdMap.end();) {
            if (it->first.first == kField) {
                _slotNameToIdMap.erase(it++);
                continue;
            }
            ++it;
        }
    }

    void clearFieldAndAllPrefixes(StringData path) {
        for (;;) {
            clear(std::pair(PlanStageSlots::kField, path));

            size_t pos = path.rfind('.');
            if (pos == std::string::npos) {
                break;
            }

            path = path.substr(0, pos);
        }
    }

    /**
     * This method applies an action to some/all of the slots within this struct. For each slot in
     * this struct, the action is will be applied to the slot if (and only if) the corresponding
     * flag in 'reqs' is true.
     */
    inline void forEachSlot(const PlanStageReqs& reqs,
                            const std::function<void(const TypedSlot&)>& fn) const;

    inline void forEachSlot(
        const PlanStageReqs& reqs,
        const std::function<void(const TypedSlot&, const UnownedSlotName&)>& fn) const;
    inline void forEachSlot(const std::function<void(const TypedSlot&)>& fn) const;
    inline void forEachSlot(
        const std::function<void(const UnownedSlotName&, const TypedSlot&)>& fn) const;
    inline void clearNonRequiredSlots(const PlanStageReqs& reqs);

private:
    // Slot type-and-name to SlotId map for the output slots produced by this plan stage.
    SlotNameMap _slotNameToIdMap;
};  // class PlanStageSlots

/**
 * The PlanStageReqs class is used by SlotBasedStageBuilder to represent the context and parent's
 * required inputs ('reqs'), which thus double as the current stage's required outputs, when
 * building a stage.
 */
class PlanStageReqs {
public:
    using SlotType = PlanStageSlots::SlotType;
    using UnownedSlotName = PlanStageSlots::UnownedSlotName;
    using OwnedSlotName = PlanStageSlots::OwnedSlotName;

    static constexpr SlotType kMeta = SlotType::kMeta;
    static constexpr SlotType kField = SlotType::kField;
    static constexpr SlotType kSortKey = SlotType::kSortKey;

    PlanStageReqs copy() const {
        return *this;
    }

    bool has(const UnownedSlotName& str) const {
        return _slotNameSet.contains(str);
    }

    PlanStageReqs& set(const UnownedSlotName& str) {
        _slotNameSet.emplace(str);
        return *this;
    }

    PlanStageReqs& set(OwnedSlotName str) {
        _slotNameSet.emplace(std::move(str));
        return *this;
    }

    PlanStageReqs& set(const std::vector<UnownedSlotName>& strs) {
        _slotNameSet.insert(strs.begin(), strs.end());
        return *this;
    }

    PlanStageReqs& set(std::vector<OwnedSlotName> strs) {
        _slotNameSet.insert(std::make_move_iterator(strs.begin()),
                            std::make_move_iterator(strs.end()));
        return *this;
    }

    PlanStageReqs& setIf(const UnownedSlotName& str, bool condition) {
        if (condition) {
            _slotNameSet.emplace(str);
        }
        return *this;
    }

    PlanStageReqs& setFields(std::vector<std::string> strs) {
        for (size_t i = 0; i < strs.size(); ++i) {
            _slotNameSet.emplace(kField, std::move(strs[i]));
        }
        return *this;
    }

    PlanStageReqs& setSortKeys(std::vector<std::string> strs) {
        for (size_t i = 0; i < strs.size(); ++i) {
            _slotNameSet.emplace(kSortKey, std::move(strs[i]));
        }
        return *this;
    }

    PlanStageReqs& clear(const UnownedSlotName& str) {
        _slotNameSet.erase(str);
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

    bool hasType(SlotType t) const {
        return std::any_of(
            _slotNameSet.begin(), _slotNameSet.end(), [t](auto& item) { return item.first == t; });
    }
    bool hasFields() const {
        return hasType(kField);
    }
    bool hasSortKeys() const {
        return hasType(kSortKey);
    }

    std::vector<std::string> getOfType(SlotType t) const {
        std::vector<std::string> res;
        for (const auto& [type, str] : _slotNameSet) {
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

    PlanStageReqs& clearAllOfType(SlotType t) {
        absl::erase_if(_slotNameSet, [t](auto& item) { return item.first == t; });
        return *this;
    }
    PlanStageReqs& clearAllFields() {
        return clearAllOfType(kField);
    }
    PlanStageReqs& clearAllSortKeys() {
        return clearAllOfType(kSortKey);
    }

    PlanStageReqs& clearFieldAndAllPrefixes(StringData path) {
        for (;;) {
            clear(std::pair(PlanStageSlots::kField, path));

            size_t pos = path.rfind('.');
            if (pos == std::string::npos) {
                break;
            }

            path = path.substr(0, pos);
        }

        return *this;
    }

    friend PlanStageSlots::PlanStageSlots(const PlanStageReqs& reqs,
                                          sbe::value::SlotIdGenerator* slotIdGenerator);

    inline void forEachReq(const std::function<void(const UnownedSlotName&)>& fn) const {
        for (const auto& name : _slotNameSet) {
            fn(name);
        }
    }

    friend PlanStageSlots::PlanStageSlots(const PlanStageReqs& reqs,
                                          sbe::value::SlotIdGenerator* slotIdGenerator);

    friend void PlanStageSlots::forEachSlot(const PlanStageReqs& reqs,
                                            const std::function<void(const TypedSlot&)>& fn) const;

    friend void PlanStageSlots::forEachSlot(
        const PlanStageReqs& reqs,
        const std::function<void(const TypedSlot&, const UnownedSlotName&)>& fn) const;

private:
    // The set of the type-and-names of the slots required as inputs by this plan stage.
    PlanStageSlots::SlotNameSet _slotNameSet;

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
};  // class PlanStageReqs

void PlanStageSlots::forEachSlot(const PlanStageReqs& reqs,
                                 const std::function<void(const TypedSlot&)>& fn) const {
    for (const auto& name : reqs._slotNameSet) {
        auto it = _slotNameToIdMap.find(name);
        tassert(7050900,
                str::stream() << "Could not find " << static_cast<int>(name.first) << ":'"
                              << name.second << "' in the slot map, expected slot to exist",
                it != _slotNameToIdMap.end());

        fn(it->second);
    }
}

void PlanStageSlots::forEachSlot(
    const PlanStageReqs& reqs,
    const std::function<void(const TypedSlot&, const UnownedSlotName&)>& fn) const {
    for (const auto& name : reqs._slotNameSet) {
        auto it = _slotNameToIdMap.find(name);
        tassert(7050901,
                str::stream() << "Could not find " << static_cast<int>(name.first) << ":'"
                              << name.second << "' in the slot map, expected slot to exist",
                it != _slotNameToIdMap.end());
        fn(it->second, name);
    }
}

void PlanStageSlots::forEachSlot(const std::function<void(const TypedSlot&)>& fn) const {
    for (const auto& entry : _slotNameToIdMap) {
        fn(entry.second);
    }
}

void PlanStageSlots::forEachSlot(
    const std::function<void(const UnownedSlotName&, const TypedSlot&)>& fn) const {
    for (const auto& entry : _slotNameToIdMap) {
        fn(entry.first, entry.second);
    }
}

void PlanStageSlots::clearNonRequiredSlots(const PlanStageReqs& reqs) {
    auto it = _slotNameToIdMap.begin();
    while (it != _slotNameToIdMap.end()) {
        auto& name = it->first;
        // We never clear kResult, regardless of whether it is required by 'reqs'.
        if (_slotNameToIdMap.key_eq()(name, kResult) || reqs.has(name)) {
            ++it;
        } else {
            _slotNameToIdMap.erase(it++);
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
    static constexpr auto kMetadataSearchSequenceToken =
        PlanStageSlots::kMetadataSearchSequenceToken;

    static constexpr auto kNothingEnvSlotName = "nothing"_sd;

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

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildUnwind(
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
};  // class SlotBasedStageBuilder
}  // namespace mongo::stage_builder
