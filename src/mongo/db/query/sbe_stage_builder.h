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
 * Returns a vector of the unique slot IDs needed by 'reqs', ordered by slot ID, and metadata slots.
 * This function is intended for use in situations where a join or sort or something else is being
 * constructed and a PlanStageSlot's contents need to be "forwarded" through a PlanStage.
 */
sbe::value::SlotVector getSlotsToForward(StageBuilderState& state,
                                         const PlanStageReqs& reqs,
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
    // The 'slotNameToIdMap' map is capable of holding different "types" of slots:
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

    using PlanStageTree = std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>;

    static constexpr SlotType kMeta = SlotType::kMeta;
    static constexpr SlotType kField = SlotType::kField;
    static constexpr SlotType kSortKey = SlotType::kSortKey;
    static constexpr SlotType kPathExpr = SlotType::kPathExpr;
    static constexpr SlotType kFilterCellField = SlotType::kFilterCellField;

    static constexpr UnownedSlotName kResult = {kMeta, "result"_sd};
    static constexpr UnownedSlotName kRecordId = {kMeta, "recordId"_sd};
    static constexpr UnownedSlotName kResultBase = {kMeta, "resultBase"_sd};
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
    static constexpr UnownedSlotName kBlockSelectivityBitmap = {kMeta, "bitmap"_sd};

    /**
     * In addition to holding individual output slots, a PlanStageSlots object can also optionally
     * contain a single "MakeResultInfo" object. (Likewise, in addition to providing APIs to ask for
     * individual named slots, PlanStageReqs provides an API to ask for a "MakeResultInfo" object.)
     *
     * Some stages, like project, work by returning a modified version of their child's result doc.
     * If we have a chain of projects (or other stages that behave similarly) and the stage at the
     * top of the chain receives a kResult req from its parent, ideally when possible we would like
     * to avoid the scenario where each stage asks its child for kResult and each stage materializes
     * a new result doc just to potentially add, modify, or drop 1 or 2 fields.
     *
     * "MakeResultInfo" provides a means for stages like project to satisfy a kResult req without
     * having to ask their child for kResult.
     *
     * Conceptually, the MakeResultInfo class can be thought of as a "package" that contains (or
     * points to) all the information that is needed to eventually materialize a result doc,
     * specifically:
     * - A kResultBase slot
     * - 0 or more kField slots
     *
     * The requirement to produce a MakeResultInfo is represented by the MakeResultInfoReq class,
     * which is defined inside PlanStageReqs.
     *
     * If a stage receives a kResult req from its parent and it decides it wants to ask its child
     * for MakeResultInfo, it adds a MakeResultInfoReq to the reqs passed to the child containing a
     * ProjectionEffects that describes how it plans to materialize the result doc. Specifically,
     * the ProjectionEffects object inside MakeResultInfoReq indicates which fields from kResultBase
     * are going to be ignored (i.e. dropped), which fields are going to be preserved as-is without
     * any modification, and which fields will be updated with a new value.
     *
     * When a stage receives a MakeResultInfo req, it can satisfy the req either by producing the
     * kResult document or by "participating" with the MakeResultInfo scheme. If a stage opts to not
     * "participate" and returns kResult, the stage builder will create a "default" MakeResultInfo
     * with kResultBase equal to kResult and an empty "modified fields" list that gets returned to
     * the parent. If a stage opts to "participate", it will copy the MakeResultInfoReq and update
     * the ProjectionEffects appropriately, and then it will pass the updated MakeResultInfoReq to
     * its child. Then it will receive MakeResultInfo object from its child, it will add the
     * appropriate field names the the MakeResultInfo's "modified fields" list, and then it will
     * return the MakeResultInfo object to its parent.
     *
     * When the stage that received a kResult req and asked for MakeResultInfo eventually receives
     * a MakeResultInfo object back from its child, it creates the kResult doc by taking the
     * contents of kResultBase as a starting point, dropping 0 or more fields from the doc, writing
     * new values (retrieved from kField slots) to 0 or more fields in the doc, and finally making
     * stage-specific modifications as appropriate to the doc and then materializing it.
     */
    class MakeResultInfo {
    public:
        static void mergeInfos(StageBuilderState& state,
                               PlanNodeId nodeId,
                               std::vector<PlanStageTree>& trees);

        MakeResultInfo() = default;

        explicit MakeResultInfo(ProjectionEffects effects) : effects(std::move(effects)) {}

        void addModifiedAndDroppedFields(const std::vector<std::string>& modifys,
                                         const std::vector<std::string>& drops) {
            // Compose 'newEffects' with 'nonMaterializedEffects' and store the result into
            // 'nonMaterializedEffects'.
            effects = ProjectionEffects(FieldSet::makeOpenSet(drops), modifys, {}).compose(effects);
        }

        ProjectionEffects effects;
    };

    struct Data {
        // Slot type-and-name to SlotId map for the output slots produced by this plan stage.
        SlotNameMap slotNameToIdMap;

        // If this PlanStageSlots object has "MakeResultInfo" set, then this field will be point to
        // a MakeResultInfo object, otherwise this field will be null.
        boost::optional<MakeResultInfo> makeResultInfo;
    };

    static std::unique_ptr<Data> cloneData(const std::unique_ptr<Data>& other) {
        if (other) {
            return std::make_unique<Data>(*other);
        }
        return {};
    }

    /**
     * When the build() depth-first traversal backtracks through a merge point in the QSN tree,
     * if getMRInfo() is non-null for one or more of the PlanStageOutputs objects,
     * then we need to do a "merge" process in order to produce a single unified non-materialized
     * result object. The makeMergedPlanStageSlots() method implements this merge process.
     */
    static PlanStageSlots makeMergedPlanStageSlots(StageBuilderState& state,
                                                   PlanNodeId nodeId,
                                                   const PlanStageReqs& reqs,
                                                   std::vector<PlanStageTree>& stagesAndSlots);

    PlanStageSlots() : _data(std::make_unique<Data>()) {}

    PlanStageSlots(const PlanStageSlots& other) : _data(cloneData(other._data)) {}

    PlanStageSlots(PlanStageSlots&& other) noexcept : _data(std::move(other._data)) {}

    ~PlanStageSlots() noexcept = default;

    PlanStageSlots& operator=(const PlanStageSlots& other) {
        if (this != &other) {
            _data = cloneData(other._data);
        }
        return *this;
    }

    PlanStageSlots& operator=(PlanStageSlots&& other) noexcept {
        if (this != &other) {
            _data = std::move(other._data);
        }
        return *this;
    }

    bool has(const UnownedSlotName& str) const {
        return _data->slotNameToIdMap.count(str);
    }

    TypedSlot get(const UnownedSlotName& str) const {
        auto it = _data->slotNameToIdMap.find(str);
        invariant(it != _data->slotNameToIdMap.end());
        return it->second;
    }

    boost::optional<TypedSlot> getIfExists(const UnownedSlotName& str) const {
        if (auto it = _data->slotNameToIdMap.find(str); it != _data->slotNameToIdMap.end()) {
            return it->second;
        }
        return boost::none;
    }

    boost::optional<sbe::value::SlotId> getSlotIfExists(const UnownedSlotName& str) const {
        if (auto it = _data->slotNameToIdMap.find(str); it != _data->slotNameToIdMap.end()) {
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
        _data->slotNameToIdMap.insert_or_assign(str, slot);
    }

    void set(OwnedSlotName str, TypedSlot slot) {
        _data->slotNameToIdMap.insert_or_assign(std::move(str), slot);
    }

    void clear(const UnownedSlotName& str) {
        _data->slotNameToIdMap.erase(str);
    }

    // Clear a single field (SlotType::kField) in 'slotNameToIdMap' by its string name.
    void clearField(StringData fieldName) {
        auto it = _data->slotNameToIdMap.find(UnownedSlotName{kField, fieldName});
        if (it != _data->slotNameToIdMap.end()) {
            _data->slotNameToIdMap.erase(it);
        }
    }

    // Clear all fields (SlotType::kField) in 'slotNameToIdMap'.
    void clearAllFields() {
        for (auto it = _data->slotNameToIdMap.begin(); it != _data->slotNameToIdMap.end();) {
            if (it->first.first == kField) {
                _data->slotNameToIdMap.erase(it++);
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

    bool hasResult() const {
        return has(kResult);
    }

    // Returns true if the kResult slot is set or if the MakeResultInfo object is set.
    bool hasResultOrMRInfo() const {
        return hasResult() || _data->makeResultInfo.has_value();
    }

    void clearResult() {
        clear(kResult);
    }

    void clearMRInfo() {
        _data->makeResultInfo.reset();
        clear(kResultBase);
    }

    MakeResultInfo* getMRInfo() {
        return _data->makeResultInfo ? &(*_data->makeResultInfo) : nullptr;
    }
    const MakeResultInfo* getMRInfo() const {
        return _data->makeResultInfo ? &(*_data->makeResultInfo) : nullptr;
    }

    // These methods set kResult and also clears MakeResultInfo. In general these methods
    // should be preferred over calling "set(kResult, slot)".
    void setResult(TypedSlot slot) {
        clearMRInfo();
        return set(kResult, slot);
    }
    void setResult(sbe::value::SlotId slot) {
        setResult(TypedSlot{slot, TypeSignature::kAnyScalarType});
    }

    // These methods set MakeResultInfo and clears kResult.
    void setMRInfoWithResultBase(TypedSlot slot) {
        clear(kResult);
        set(kResultBase, slot);
        _data->makeResultInfo.emplace(ProjectionEffects());
    }
    void setMRInfoWithResultBase(sbe::value::SlotId slot) {
        setMRInfoWithResultBase(TypedSlot{slot, TypeSignature::kAnyScalarType});
    }

    std::vector<OwnedSlotName> getRequiredNamesInOrder(const PlanStageReqs& reqs) const;

    std::vector<TypedSlot> getRequiredSlotsInOrder(const PlanStageReqs& reqs) const;

    std::vector<TypedSlot> getRequiredSlotsUnique(const PlanStageReqs& reqs) const;

    std::vector<TypedSlot> getAllSlotsInOrder() const;

    std::vector<std::pair<UnownedSlotName, TypedSlot>> getAllNamedSlotsInOrder() const;

    void setMissingRequiredNamedSlotsToNothing(StageBuilderState& state, const PlanStageReqs& reqs);

    void clearNonRequiredSlots(const PlanStageReqs& reqs, bool saveResultSlot = true);

private:
    std::unique_ptr<Data> _data;
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
    static constexpr SlotType kPathExpr = SlotType::kPathExpr;

    /**
     * In addition to providing APIs to ask for individual named slots, PlanStageReqs provides an
     * API to ask for a "MakeResultInfo" object. The requirement to produce a MakeResultInfo object
     * is called a "MakeResultInfo req" and is represented using the 'MakeResultInfoReq' class.
     */
    class MakeResultInfoReq {
    public:
        MakeResultInfoReq() = default;

        explicit MakeResultInfoReq(FieldSet allowedSet) : allowedSet(std::move(allowedSet)) {}

        FieldSet getNeededFieldSet() const {
            return allowedSet;
        }

        FieldSet allowedSet;
    };

    struct Data {
        // The set of the type-and-names of the slots required as inputs by this plan stage.
        PlanStageSlots::SlotNameSet slotNameSet;

        // If this PlanStageReqs object has a "MakeResultInfo" req set, then this field will be
        // point to a MakeResultInfoReq object, otherwise this field will be null.
        boost::optional<MakeResultInfoReq> makeResultInfoReq;

        // When we're in the middle of building a special union sub-tree implementing a tailable
        // cursor collection scan, this flag will be set to true. Otherwise this flag will be false.
        bool isBuildingUnionForTailableCollScan{false};

        // When we're in the middle of building a special union sub-tree implementing a tailable
        // cursor collection scan, this flag indicates whether we're currently building an anchor or
        // resume branch. At all other times, this flag will be false.
        bool isTailableCollScanResumeBranch{false};

        // When we are processing a stage that can work on top of block values, this flag instruct
        // the child stage not to insert a BlockToRow stage to convert the block values into scalar
        // values.
        bool canProcessBlockValues{false};

        // Tracks the current namespace that we're building a plan over. Given that the stage
        // builder can build plans for multiple namespaces, a node in the tree that targets a
        // namespace different from its parent node can set this value to notify any child nodes of
        // the correct namespace.
        NamespaceString targetNamespace;

        // When the pipeline has a limit stage this will be set to true. The flag is used by the
        // sort stage to improve the performance of queries that have both sort and limit.
        bool hasLimit{false};
    };

    static std::unique_ptr<Data> cloneData(const std::unique_ptr<Data>& other) {
        if (other) {
            return std::make_unique<Data>(*other);
        }
        return {};
    }

    PlanStageReqs() : _data(std::make_unique<Data>()) {}

    PlanStageReqs(const PlanStageReqs& other) : _data(cloneData(other._data)) {}

    PlanStageReqs(PlanStageReqs&& other) noexcept : _data(std::move(other._data)) {}

    ~PlanStageReqs() noexcept = default;

    PlanStageReqs& operator=(const PlanStageReqs& other) {
        if (this != &other) {
            _data = cloneData(other._data);
        }
        return *this;
    }

    PlanStageReqs& operator=(PlanStageReqs&& other) noexcept {
        if (this != &other) {
            _data = std::move(other._data);
        }
        return *this;
    }

    PlanStageReqs copyForChild() const {
        PlanStageReqs copy = *this;
        // The flag to signal that block processing is supported must be be explicitly set to true
        // by the code handling each block-enabled stage.
        copy.setCanProcessBlockValues(false);
        return copy;
    }

    bool has(const UnownedSlotName& str) const {
        return _data->slotNameSet.contains(str);
    }

    PlanStageReqs& set(const UnownedSlotName& str) {
        _data->slotNameSet.emplace(str);
        return *this;
    }

    PlanStageReqs& set(OwnedSlotName str) {
        _data->slotNameSet.emplace(std::move(str));
        return *this;
    }

    PlanStageReqs& set(const std::vector<UnownedSlotName>& strs) {
        _data->slotNameSet.insert(strs.begin(), strs.end());
        return *this;
    }

    PlanStageReqs& set(std::vector<OwnedSlotName> strs) {
        _data->slotNameSet.insert(std::make_move_iterator(strs.begin()),
                                  std::make_move_iterator(strs.end()));
        return *this;
    }

    PlanStageReqs& setIf(const UnownedSlotName& str, bool condition) {
        if (condition) {
            _data->slotNameSet.emplace(str);
        }
        return *this;
    }

    PlanStageReqs& setFields(std::vector<std::string> strs) {
        for (size_t i = 0; i < strs.size(); ++i) {
            _data->slotNameSet.emplace(kField, std::move(strs[i]));
        }
        return *this;
    }

    PlanStageReqs& setSortKeys(std::vector<std::string> strs) {
        for (size_t i = 0; i < strs.size(); ++i) {
            _data->slotNameSet.emplace(kSortKey, std::move(strs[i]));
        }
        return *this;
    }

    PlanStageReqs& clear(const UnownedSlotName& str) {
        _data->slotNameSet.erase(str);
        return *this;
    }

    bool hasType(SlotType t) const {
        return std::any_of(_data->slotNameSet.begin(), _data->slotNameSet.end(), [t](auto& item) {
            return item.first == t;
        });
    }

    bool hasFields() const {
        return hasType(kField);
    }

    bool hasSortKeys() const {
        return hasType(kSortKey);
    }

    std::vector<std::string> getOfType(SlotType t) const {
        std::vector<std::string> res;
        for (const auto& [type, str] : _data->slotNameSet) {
            if (type == t) {
                res.push_back(str);
            }
        }
        std::sort(res.begin(), res.end());
        return res;
    }

    /**
     * Returns the list of fields that are explicitly required to be put into individual kField
     * slots.
     *
     * Note that the list returned is not an exhaustive list of every field that might be
     * needed, nor is it an exhaustive list of all the fields that will ultimately be required
     * to be put into kField slots (since MakeResultInfo provides a mechanism that allows
     * the child to "implicitly" require additional fields to be put in kField slots).
     * For detais, see getNeededFieldSet().
     */
    std::vector<std::string> getFields() const {
        return getOfType(kField);
    }

    std::vector<std::string> getSortKeys() const {
        return getOfType(kSortKey);
    }

    /**
     * Returns the set of all field names that are potentially needed (either in an individual
     * kField slots or stored within kResult / kResultBase).
     *
     * This method returns a FieldSet. A FieldSet can either be a finite set (if scope == kClosed)
     * or it can an infinite set that is the complement of some finite set (if scope == kOpen).
     *
     * If hasResult() is true, this method always returns the set of all possible fields names.
     * Otherwise, this method returns getFields() unioned with the set of fields needed by the
     * MakeResultInfo req (if it is set).
     */
    FieldSet getNeededFieldSet() const;

    PlanStageReqs& clearAllOfType(SlotType t) {
        absl::erase_if(_data->slotNameSet, [t](auto& item) { return item.first == t; });
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

    bool hasResult() const {
        return has(PlanStageSlots::kResult);
    }

    // Returns true if the kResult req is set or if the MakeResultInfo req is set.
    bool hasResultOrMRInfo() const {
        return hasResult() || _data->makeResultInfoReq.has_value();
    }

    PlanStageReqs& clearResult() {
        return clear(PlanStageSlots::kResult);
    }

    PlanStageReqs& clearMRInfo() {
        _data->makeResultInfoReq.reset();
        clear(PlanStageSlots::kResultBase);
        return *this;
    }

    MakeResultInfoReq* getMRInfo() {
        return _data->makeResultInfoReq ? &(*_data->makeResultInfoReq) : nullptr;
    }
    const MakeResultInfoReq* getMRInfo() const {
        return _data->makeResultInfoReq ? &(*_data->makeResultInfoReq) : nullptr;
    }

    // This method sets kResult and also clears MakeResultInfo. In general this method
    // should be preferred over calling "set(kResult)".
    PlanStageReqs& setResult() {
        clearMRInfo();
        return set(PlanStageSlots::kResult);
    }

    // If 'has(kResult)' is false then this method will set MakeResultInfo, otherwise this
    // method will do nothing.
    //
    // To set MakeResultInfo unconditionally, call clearResult() first before calling this method.
    PlanStageReqs& setMRInfo(const FieldSet& allowedSet) {
        if (!has(PlanStageSlots::kResult)) {
            _data->makeResultInfoReq.emplace(allowedSet);
            _data->slotNameSet.emplace(PlanStageSlots::kResultBase);
        }
        return *this;
    }

    bool getIsBuildingUnionForTailableCollScan() const {
        return _data->isBuildingUnionForTailableCollScan;
    }

    PlanStageReqs& setIsBuildingUnionForTailableCollScan(bool b) {
        _data->isBuildingUnionForTailableCollScan = b;
        return *this;
    }

    bool getIsTailableCollScanResumeBranch() const {
        return _data->isTailableCollScanResumeBranch;
    }

    PlanStageReqs& setIsTailableCollScanResumeBranch(bool b) {
        _data->isTailableCollScanResumeBranch = b;
        return *this;
    }

    bool getCanProcessBlockValues() const {
        return _data->canProcessBlockValues;
    }

    PlanStageReqs& setCanProcessBlockValues(bool b) {
        _data->canProcessBlockValues = b;
        return *this;
    }

    PlanStageReqs& setTargetNamespace(const NamespaceString& nss) {
        _data->targetNamespace = nss;
        return *this;
    }

    const NamespaceString& getTargetNamespace() const {
        return _data->targetNamespace;
    }

    bool getHasLimit() const {
        return _data->hasLimit;
    }

    PlanStageReqs& setHasLimit(bool b) {
        _data->hasLimit = b;
        return *this;
    }

private:
    friend class PlanStageSlots;

    std::unique_ptr<Data> _data;
};  // class PlanStageReqs

struct BuildProjectionPlan {
    enum Type {
        kDoNotMakeResult,
        kUseCoveredProjection,
        kUseInputPlanWithoutObj,
        kUseChildResultObj,
        kUseChildResultInfo
    };

    PlanStageReqs childReqs;
    Type type;
    const PlanStageReqs::MakeResultInfoReq* reqMRInfo;
    bool produceMaterializedResult;
    bool isInclusion;
    std::vector<std::string> paths;
    std::vector<ProjectNode> nodes;
    std::vector<std::string> nothingPaths;
    std::vector<std::string> resultPaths;
    std::vector<std::string> updatedPaths;
    StringMap<Expression*> updatedPathsExprMap;
    std::vector<std::string> mrInfoModifys;
    std::vector<std::string> mrInfoDrops;
    std::vector<std::string> projNothingInputFields;
    boost::optional<std::vector<std::string>> inputPlanSingleFields;
};

/**
 * We use one of these structs per node in the QSN tree to store the results of the
 * analyze() phase.
 */
struct QsnAnalysis {
    FieldSet allowedFieldSet = FieldSet::makeUniverseSet();
};

/**
 * A stage builder which builds an executable tree using slot-based PlanStages.
 */
class SlotBasedStageBuilder final
    : public StageBuilder<std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageData>> {
public:
    using PlanType = std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageData>;
    using BaseType = StageBuilder<PlanType>;

    static constexpr auto kMeta = PlanStageSlots::SlotType::kMeta;
    static constexpr auto kField = PlanStageSlots::SlotType::kField;
    static constexpr auto kSortKey = PlanStageSlots::SlotType::kSortKey;

    static constexpr auto kResult = PlanStageSlots::kResult;
    static constexpr auto kRecordId = PlanStageSlots::kRecordId;
    static constexpr auto kResultBase = PlanStageSlots::kResultBase;
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
    void analyzeTree(const QuerySolutionNode* node);

    QsnAnalysis analyze(const QuerySolutionNode* node);

    inline const QsnAnalysis& getAnalysis(const QuerySolutionNode* node) const {
        return _analysis.find(node)->second;
    }

    inline const QsnAnalysis& getAnalysis(const std::unique_ptr<QuerySolutionNode>& node) const {
        return _analysis.find(node.get())->second;
    }

    const FieldSet& getAllowedFieldSet(const QuerySolutionNode* node) {
        analyzeTree(node);
        return getAnalysis(node).allowedFieldSet;
    }

    const FieldSet& getAllowedFieldSet(const std::unique_ptr<QuerySolutionNode>& node) {
        analyzeTree(node.get());
        return getAnalysis(node).allowedFieldSet;
    }

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildTree();

    /**
     * This method will build an SBE PlanStage tree for QuerySolutionNode 'root' and its
     * descendents.
     *
     * Based on the type of 'root', this method will dispatch to the appropriate buildXXX()
     * method. This method will also handle generating calls to getField() to satisfy kField
     * reqs that were not satisfied by the buildXXX() method.
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

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildProjection(
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
     * slots can be read directly in order to determine the shard key that should be passed to
     * the 'shardFiltererSlot'.
     */
    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildShardFilterCovered(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildGroup(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildGroupImpl(
        const GroupNode* groupNode,
        const PlanStageReqs& reqs,
        std::unique_ptr<sbe::PlanStage> childStage,
        PlanStageSlots childOutputs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildLookup(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildUnpackTsBucket(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::unique_ptr<BuildProjectionPlan> makeBuildProjectionPlan(const QuerySolutionNode* root,
                                                                 const PlanStageReqs& reqs);

    std::unique_ptr<sbe::PlanStage> buildBlockToRow(std::unique_ptr<sbe::PlanStage> stage,
                                                    PlanStageSlots& outputs);

    // Given an expression built on top of scalar processing, along with the definition of the
    // visible slots (some of which could be marked as holding block of values), produce an
    // expression tree that can be executed directly on top of them. Returns an empty result if the
    // expression isn't vectorizable.
    boost::optional<TypedExpression> buildVectorizedExpr(SbExpr scalarExpression,
                                                         PlanStageSlots& outputs,
                                                         bool forFilterStage);

    std::unique_ptr<sbe::EExpression> buildLimitSkipAmountExpression(
        LimitSkipParameterization canBeParameterized,
        long long amount,
        boost::optional<sbe::value::SlotId>& slot);
    std::unique_ptr<sbe::EExpression> buildLimitSkipSumExpression(
        LimitSkipParameterization canBeParameterized, long long limitSkipSum);

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
    // and the PlanStageStaticData. Note that the PlanStageStaticData ('_data') is mutable
    // inside SlotBasedStageBuilder, but after the 'build(const QuerySolutionNode*)' method is
    // called the data will become immutable.
    Environment _env;
    std::unique_ptr<PlanStageStaticData> _data;

    const QuerySolutionNode* _root{nullptr};
    absl::node_hash_map<const QuerySolutionNode*, QsnAnalysis> _analysis;

    bool _buildHasStarted{false};

    // Common parameters to SBE stage builder functions.
    StageBuilderState _state;
};  // class SlotBasedStageBuilder

}  // namespace mongo::stage_builder
