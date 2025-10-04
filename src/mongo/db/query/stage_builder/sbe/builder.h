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

#include "mongo/base/string_data.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/collection_helpers.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/trial_period_utils.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/shard_filterer_factory_interface.h"
#include "mongo/db/query/stage_builder/sbe/analysis.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/type_signature.h"
#include "mongo/db/query/stage_builder/stage_builder.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/hash/hash.h>
#include <absl/meta/type_traits.h>
#include <absl/strings/string_view.h>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::stage_builder {

class PlanStageReqs;
class PlanStageSlots;

struct PlanStageData;

/**
 * Returns a vector of the unique slot IDs needed by 'reqs', ordered by slot ID, and metadata slots.
 * This function is intended for use in situations where a join or sort or something else is being
 * constructed and a PlanStageSlot's contents need to be "forwarded" through a PlanStage.
 */
SbSlotVector getSlotsToForward(StageBuilderState& state,
                               const PlanStageReqs& reqs,
                               const PlanStageSlots& outputs,
                               const SbSlotVector& exclude = SbSlotVector{});

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

std::pair<SbStage, PlanStageData> buildSearchMetadataExecutorSBE(OperationContext* opCtx,
                                                                 const ExpressionContext& expCtx,
                                                                 size_t remoteCursorId,
                                                                 RemoteCursorMap* remoteCursors,
                                                                 PlanYieldPolicySBE* yieldPolicy);

/**
 * The PlanStageSlots class is used by SlotBasedStageBuilder to return the output slots produced
 * after building a stage.
 */
class PlanStageSlots {
public:
    // The 'slotNameToIdMap' map is capable of holding different "types" of slots:
    // 1) kMeta slots are used to hold the current document, record ID, and various pieces of
    //    metadata.
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

    using MakeMergeStageFn = std::function<std::pair<SbStage, SbSlotVector>(
        sbe::PlanStage::Vector, std::vector<SbSlotVector>)>;

    struct NameHasher {
        using is_transparent = void;
        size_t operator()(const UnownedSlotName& p) const noexcept {
            auto h{std::pair{p.first, absl::string_view{p.second.data(), p.second.size()}}};
            return absl::Hash<decltype(h)>{}(h);
        }
    };

    struct NameEq : std::equal_to<UnownedSlotName> {
        using is_transparent = void;
    };

    using SlotNameMap = absl::flat_hash_map<OwnedSlotName, SbSlot, NameHasher, NameEq>;
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
    static constexpr UnownedSlotName kPrefetchedResult = {kMeta, "prefetchedResult"_sd};
    static constexpr UnownedSlotName kMetadataSearchScore = {kMeta, "metadataSearchScore"_sd};
    static constexpr UnownedSlotName kMetadataSearchHighlights = {kMeta,
                                                                  "metadataSearchHighlights"_sd};
    static constexpr UnownedSlotName kMetadataSearchDetails = {kMeta, "metadataSearchDetails"_sd};
    static constexpr UnownedSlotName kMetadataSearchSortValues = {kMeta,
                                                                  "metadataSearchSortValues"_sd};
    static constexpr UnownedSlotName kMetadataSearchSequenceToken = {
        kMeta, "metadataSearchSequenceToken"_sd};
    static constexpr UnownedSlotName kBlockSelectivityBitmap = {kMeta, "bitmap"_sd};

    struct Data {
        // Slot type-and-name to SlotId map for the output slots produced by this plan stage.
        SlotNameMap slotNameToIdMap;

        // If this PlanStageSlots has a ResultInfo then 'resultInfoChanges' will hold the recorded
        // changes for the ResultInfo, otherwise it will be set to boost::none.
        boost::optional<FieldEffects> resultInfoChanges;
    };

    static std::unique_ptr<Data> cloneData(const std::unique_ptr<Data>& other) {
        if (other) {
            return std::make_unique<Data>(*other);
        }
        return {};
    }

    /**
     * When the build() depth-first traversal backtracks through a merge point in the QSN tree, this
     * method is called. This method merges multiple SbStage/PlanStageSlots pairs ('stagesAndSlots')
     * into a single SbStage/PlanStageSlots pair, and then it returns the pair.
     *
     * The caller must provide a 'makeMergeStageFn' lambda. This lambda is responsible for creating
     * the appropriate stage for the merge (ex. UnionStage, BranchStage, SortedMergeStage, etc).
     */
    static std::pair<SbStage, PlanStageSlots> makeMergedPlanStageSlots(
        StageBuilderState& state,
        PlanNodeId nodeId,
        const PlanStageReqs& reqs,
        std::vector<std::pair<SbStage, PlanStageSlots>> stagesAndSlots,
        const MakeMergeStageFn& makeMergeStageFn,
        const std::vector<const FieldSet*>& allowedFieldSets = {});

    // This is a helper function used by makeMergedPlanStageSlots() is called that handles the
    // case where one or more of the PlanStageOutputs objects have a ResultInfo.
    static void mergeResultInfos(StageBuilderState& state,
                                 PlanNodeId nodeId,
                                 const PlanStageReqs& reqs,
                                 std::vector<std::pair<SbStage, PlanStageSlots>>& trees,
                                 const std::vector<const FieldSet*>& allowedFieldSets);

    PlanStageSlots() : _data(std::make_unique<Data>()) {}

    PlanStageSlots(const PlanStageSlots& other) : _data(cloneData(other._data)) {}

    PlanStageSlots(PlanStageSlots&& other) noexcept : _data(std::move(other._data)) {}

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

    // Returns true if this PlanStageSlots has a mapping for 'name'.
    bool has(const UnownedSlotName& name) const {
        return _data->slotNameToIdMap.count(name);
    }

    // Returns the slot corresponding to 'name'. This method will raise a tassert if
    // this PlanStageSlots does not have a mapping for 'name'.
    SbSlot get(const UnownedSlotName& name) const {
        auto it = _data->slotNameToIdMap.find(name);
        tassert(11051830,
                str::stream() << "PlanStageSlots does not have a mapping for name " << name.second,
                it != _data->slotNameToIdMap.end());
        return it->second;
    }

    // Returns the slot corresponding to 'name' if this PlanStageSlot has a mapping for 'name',
    // otherwise returns boost::none.
    boost::optional<SbSlot> getIfExists(const UnownedSlotName& name) const {
        if (auto it = _data->slotNameToIdMap.find(name); it != _data->slotNameToIdMap.end()) {
            return it->second;
        }
        return boost::none;
    }

    // Maps 'name' to 'slot' and clears any prior mapping the 'name' may have had.
    void set(const UnownedSlotName& name, SbSlot slot) {
        _data->slotNameToIdMap.insert_or_assign(name, slot);
    }
    void set(OwnedSlotName name, SbSlot slot) {
        _data->slotNameToIdMap.insert_or_assign(std::move(name), slot);
    }

    // Discards any mapping that this PlanStageSlot may have for 'name'.
    void clear(const UnownedSlotName& name) {
        _data->slotNameToIdMap.erase(name);
    }

    // Equivalent to 'clear({kField, fieldName})'.
    void clearField(StringData fieldName) {
        clear(UnownedSlotName(kField, fieldName));
    }

    // Discards all mappings for names of the form '{kField, name}'.
    void clearAllFields() {
        absl::erase_if(_data->slotNameToIdMap, [](auto& elem) {
            const auto& name = elem.first;
            return name.first == kField;
        });
    }

    // This method will clear all fields whose names conflict with 'path' (i.e. either the name
    // equals 'path', or the name is a prefix of 'path', or 'path' is a prefix of the name).
    void clearAffectedFields(StringData path) {
        absl::erase_if(_data->slotNameToIdMap, [path](auto& elem) {
            const auto& name = elem.first;
            return name.first == kField && pathsAreConflicting(name.second, path);
        });
    }

    /**
     * PlanStageSlots implements special behavior for kResult. At any given time a PlanStageSlots
     * may map kResult to a materialized result object (if hasResultObj() == true), or map kResult
     * to a base object that is a part of a "ResultInfo" (if hasResultInfo() == true), or not have
     * a kResult mapping (if hasResultObj() == false and hasResultInfo() == false). (It's impossible
     * for kResult to map to both a materialized result object and a ResultInfo base object at the
     * same time, so hasResultObj() and hasResultInfo() can never both be true simultaneously.)
     *
     * "ResultInfo" can be thought of as a package containing all the information that's needed to
     * eventually materialize a result doc, specifically: (1) a "result base object" stored in the
     * kResult slot; (2) a list of changed kField names ('resultInfoChanges'); and (3) a list of
     * slots corresponding to the changed kField names.
     *
     * If a stage receives a materialized result req from its parent and it decides it wants to ask
     * its child for ResultInfo, it adds a ResultInfo req to the reqs passed to the child containing
     * a FieldSet that describes how it plans to materialize the result doc. Specifically, the
     * FieldSet object indicates which fields from result base object (stored in the kResult slot)
     * are going to be ignored (i.e. dropped).
     *
     * When a stage receives a ResultInfo req, it can satisfy the req either by producing the
     * materialized result object or by "participating" with the ResultInfo's scheme. If a stage
     * opts to not "participate" and returns a materialized result object, the stage builder
     * will create a "default" ResultInfo to the materialized result object and an "all-Keep"
     * 'resultInfoChanges' that gets returned to the parent. If a stage opts to "participate",
     * it will copy the ResultInfo req and update the FieldSet appropriately, and then it will
     * pass the updated ResultInfo req to its child. Then it will receive a ResultInfo base object
     * (and 0 or more individual kField slots) from its child, it will add the appropriate field
     * names from 'resultInfoChanges', and then it will return the ResultInfo base object (and 0
     * or more kField slots) to its parent.
     *
     * When the stage that received a materialized result req from its parent and asked its child
     * for ResultInfo receives the ResultInfo base object back from its child, it creates the
     * materialized result object by taking the contents of the ResultInfo base object as a
     * starting point, dropping 0 or more fields from the doc, writing new values (retrieved from
     * kField slots) to 0 or more fields in the doc, and finally making stage-specific modifications
     * to the doc as appropriate and then materializing it.
     */

    // Returns true if this PlanStageSlots has a mapping for kResult. Note that kResult may map to
    // a materialized result object -OR- it may map to a "ResultInfo" base object. To distinguish
    // between these two cases, use the hasResultObj() or hasResultInfo() methods.
    bool hasResult() const {
        return has(kResult);
    }

    // This method clears any mapping kResult may have had, and it clears any ResultInfo-related
    // state that may have been set. After this method returns, hasResult() and hasResultObject()
    // and hasResultInfo() will all be false.
    void clearResult() {
        clear(kResult);
        _data->resultInfoChanges.reset();
    }

    // Returns true if this PlanStageSlots has kResult and it maps to a materialized result object,
    // otherwise returns false.
    bool hasResultObj() const {
        return has(kResult) && !_data->resultInfoChanges.has_value();
    }

    bool hasBlockOutput() const {
        return has(kBlockSelectivityBitmap);
    }

    // Returns the slot that holds the materialized result object. This method will tassert
    // if 'hasResultObj()' is false.
    SbSlot getResultObj() const {
        tassert(8428000, "Expected result object to be set", hasResultObj());

        auto it = _data->slotNameToIdMap.find(kResult);

        return it->second;
    }

    // If 'hasResultObj()' is true this method returns the slot that holds the materialized result
    // object, otherwise it will return boost::none.
    boost::optional<SbSlot> getResultObjIfExists() const {
        if (hasResultObj()) {
            return getResultObj();
        }
        return boost::none;
    }

    // Maps kResult to 'slot', designates kResult as being a "materialized result object", and
    // clears any prior mapping or designation that kResult may have had. setResultObj() also
    // clears any ResultInfo-related state that may have been set.
    void setResultObj(SbSlot slot) {
        set(kResult, slot);
        _data->resultInfoChanges.reset();
    }

    // Returns true if this PlanStageSlots has "ResultInfo" (kResult mapped to a base object, a list
    // of changed fields in 'resultInfoChanges', and changed field slots), otherwise returns false.
    bool hasResultInfo() const {
        return _data->resultInfoChanges.has_value();
    }

    // Returns the slot that holds the ResultInfo base object. This method will tassert if
    // 'hasResultInfo()' is false.
    SbSlot getResultInfoBaseObj() const {
        tassert(8428001, "Expected ResultInfo to be set", hasResultInfo());

        auto it = _data->slotNameToIdMap.find(kResult);
        tassert(11051829,
                "slotNameToIdMap is missing the 'result' mapping",
                it != _data->slotNameToIdMap.end());

        return it->second;
    }

    // If 'hasResultInfo()' is true this method returns the slot that holds the ResultInfo base
    // object, otherwise it will return boost::none.
    boost::optional<SbSlot> getResultInfoBaseObjIfExists() const {
        if (hasResultInfo()) {
            return getResultInfoBaseObj();
        }
        return boost::none;
    }

    // Maps kResult to 'slot', designates kResult as being as a "ResultInfo base object", clears
    // any prior mapping or designation that kResult may have had, and sets 'resultInfoChanges' to
    // hold an empty list of changed fields.
    void setResultInfoBaseObj(SbSlot slot) {
        set(kResult, slot);
        _data->resultInfoChanges.emplace();
    }

    // Returns the list of changed fields stored in 'resultInfoChanges'. This method will tassert
    // if 'hasResultInfo()' is false.
    const FieldEffects& getResultInfoChanges() const {
        tassert(8428002, "Expected ResultInfo to be set", hasResultInfo());

        return *_data->resultInfoChanges;
    }

    // Adds the non-Keep effects from 'newEffects' to 'resultInfoChanges'. This method will tassert
    // if 'hasResultInfo()' is false.
    void addEffectsToResultInfo(StageBuilderState& state,
                                const PlanStageReqs& reqs,
                                const FieldEffects& newEffectsIn);

    // These methods take a vector of slot names and return the corresponding list of slots. The
    // elements of the vector returned will correspond pair-wise with the elements of the input
    // vector.
    SbSlotVector getSlotsByName(const std::vector<PlanStageSlots::UnownedSlotName>& names) const;

    SbSlotVector getSlotsByName(const std::vector<PlanStageSlots::OwnedSlotName>& names) const;

    // Returns a sorted list of all the names in this PlanStageSlots has that are required by
    // 'reqs', plus any additional names needed by 'reqs' that this PlanStageSlots does not satisfy.
    std::vector<OwnedSlotName> getRequiredNamesInOrder(const PlanStageReqs& reqs) const;

    // Returns a list of slots that correspond pairwise to the list of names produced by calling
    // 'getRequiredNamesInOrder(reqs)'.
    SbSlotVector getRequiredSlotsInOrder(const PlanStageReqs& reqs) const;

    // This method returns a de-dupped list of all the slots that correspond to the names produced
    // calling by 'getRequiredNamesInOrder(reqs)'. The list returned by this method is sorted by
    // slot ID.
    SbSlotVector getRequiredSlotsUnique(const PlanStageReqs& reqs) const;

    // Returns a sorted list of all the name->slot mappings in this PlanStageSlots, sorted by
    // slot name.
    std::vector<std::pair<UnownedSlotName, SbSlot>> getAllNameSlotPairsInOrder() const;

    // This method calls getAllNameSlotPairsInOrder() and then returns a list of the slots only,
    // sorted by slot name.
    SbSlotVector getAllSlotsInOrder() const;

    // This method computes the list of required names 'L = getRequiredNamesInOrder(reqs)', and
    // then it adds a mapping 'N->NothingSlot' for each name N in list L where 'has(N)' is false.
    void setMissingRequiredNamedSlotsToNothing(StageBuilderState& state, const PlanStageReqs& reqs);

    // Removes all names (and their corresponding mappings) from this PlanStageSlots that do not
    // appear in list produced by calling 'getRequiredNamesInOrder(reqs)'.
    void clearNonRequiredSlots(const PlanStageReqs& reqs, bool saveResultObj = true);

    const SlotNameMap& getSlotNameToIdMap() const {
        return _data->slotNameToIdMap;
    }

private:
    template <typename T>
    SbSlotVector getSlotsByNameImpl(const T& names) const {
        SbSlotVector result;

        for (const auto& name : names) {
            auto it = _data->slotNameToIdMap.find(name);
            tassert(8146615,
                    str::stream() << "Could not find " << static_cast<int>(name.first) << ":'"
                                  << name.second << "' in the slot map, expected slot to exist",
                    it != _data->slotNameToIdMap.end());

            result.emplace_back(it->second);
        }

        return result;
    }

    std::unique_ptr<Data> _data;
};  // class PlanStageSlots

std::ostream& operator<<(std::ostream& os, const PlanStageSlots::SlotType& slotType);
std::ostream& operator<<(std::ostream& os, const PlanStageSlots& slots);

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

    struct Data {
        // The set of the type-and-names of the slots required as inputs by this plan stage.
        PlanStageSlots::SlotNameSet slotNameSet;

        boost::optional<FieldSet> trackedFieldSet;
        boost::optional<FieldEffects> resultInfoEffects;

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

    // Returns the number of slots requested.
    bool size() const {
        return _data->slotNameSet.size();
    }

    // Returns true if this PlanStageReqs has an explicit requirement for 'name'.
    bool has(const UnownedSlotName& name) const {
        return _data->slotNameSet.contains(name);
    }

    // Adds an explicit requirement for 'name'.
    PlanStageReqs& set(const UnownedSlotName& name) {
        _data->slotNameSet.emplace(name);
        return *this;
    }
    PlanStageReqs& set(OwnedSlotName name) {
        _data->slotNameSet.emplace(std::move(name));
        return *this;
    }

    // Adds an explicit requirement for each name in 'names'.
    PlanStageReqs& set(const std::vector<UnownedSlotName>& names) {
        _data->slotNameSet.insert(names.begin(), names.end());
        return *this;
    }
    PlanStageReqs& set(std::vector<OwnedSlotName> names) {
        _data->slotNameSet.insert(std::make_move_iterator(names.begin()),
                                  std::make_move_iterator(names.end()));
        return *this;
    }

    // This method adds an explicit requirement for 'name' if 'condition' is true, otherwise this
    // method does nothing.
    PlanStageReqs& setIf(const UnownedSlotName& str, bool condition) {
        if (condition) {
            _data->slotNameSet.emplace(str);
        }
        return *this;
    }

    // This method adds an explicit requirement for '{kField, N}' for each string N in 'names'.
    PlanStageReqs& setFields(std::vector<std::string> names) {
        for (size_t i = 0; i < names.size(); ++i) {
            _data->slotNameSet.emplace(kField, std::move(names[i]));
        }
        return *this;
    }

    // This method adds an explicit requirement for '{kSortKey, N}' for each string N in 'names'.
    PlanStageReqs& setSortKeys(std::vector<std::string> names) {
        for (size_t i = 0; i < names.size(); ++i) {
            _data->slotNameSet.emplace(kSortKey, std::move(names[i]));
        }
        return *this;
    }

    // Removes any explicit requirement for 'name' that this PlanStageReqs may have.
    PlanStageReqs& clear(const UnownedSlotName& str) {
        _data->slotNameSet.erase(str);
        return *this;
    }

    // Returns true if this PlanStageReqs has at least one explicit requirement '{kField, N}' for
    // some string N.
    bool hasFields() const {
        return hasType(kField);
    }

    // Returns true if this PlanStageReqs has at least one explicit requirement '{kSortKey, N}' for
    // some string N.
    bool hasSortKeys() const {
        return hasType(kSortKey);
    }

    // Returns a list of all strings N where 'has({kField, N})' is true, sorted in lexicographic
    // order.
    //
    // Note that the list returned will only contain explicit '{kField, N}' requirements. To get
    // a list of all explicit and implicit '{kField, N}' requiremnts, use getNeededFieldSet().
    std::vector<std::string> getFields() const {
        return getOfType(kField);
    }

    // Returns a list of all strings N where 'has({kSortKey, N})' is true, sorted in lexicographic
    // order.
    std::vector<std::string> getSortKeys() const {
        return getOfType(kSortKey);
    }

    // Returns a list of all strings N where 'has({kPathExpr, N})' is true, sorted in lexicographic
    // order.
    std::vector<std::string> getPathExprs() const {
        return getOfType(kPathExpr);
    }

    // Returns a FieldSet containing all strings N where 'has({kField, N})' is true plus all
    // strings M where the value of result field is needed to satisfy this PlanStageReqs's result
    // object requirement or ResultInfo requirement (if it has either).
    //
    // This method returns a FieldSet. A FieldSet can either be a finite set (if scope == kClosed)
    // or it can an infinite set that is the complement of some finite set (if scope == kOpen).
    FieldSet getNeededFieldSet() const;

    // Equivalent to 'clear({kField, fieldName})'.
    void clearField(StringData fieldName) {
        clear(UnownedSlotName(kField, fieldName));
    }

    // Clears any requirements of the form '{kField, name}' that this PlanStageReqs may have.
    PlanStageReqs& clearAllFields() {
        return clearAllOfType(kField);
    }

    // Clears any requirements of the form '{kSortKey, name}' that this PlanStageReqs may have.
    PlanStageReqs& clearAllSortKeys() {
        return clearAllOfType(kSortKey);
    }

    // This method will clear all field reqs whose names are conflict with 'path' (i.e. either the
    // name equals 'path', or the name is a prefix of 'path', or 'path' is a prefix of the name).
    PlanStageReqs& clearAffectedFields(StringData path) {
        absl::erase_if(_data->slotNameSet, [path](auto& name) {
            return name.first == kField && pathsAreConflicting(name.second, path);
        });
        return *this;
    }

    // Returns true if this PlanStageReqs has an explicit requirement for a materialized result
    // object or a ResultInfo. To distinguish between these two cases, use the hasResultObj() or
    // hasResultInfo() methods.
    bool hasResult() const {
        return has(PlanStageSlots::kResult) || _data->resultInfoEffects.has_value();
    }

    // This method clears any result object or ResultInfo requirements that this PlanStageReqs may
    // have had, and it also clears any ResultInfo-related state that may have been set. After this
    // method returns, hasResult() and hasResultObject() and hasResultInfo() will all be false.
    PlanStageReqs& clearResult() {
        _data->trackedFieldSet.reset();
        _data->resultInfoEffects.reset();
        clear(PlanStageSlots::kResult);
        return *this;
    }

    // Returns true if this PlanStageReqs requires a materialized result object, otherwise returns
    // false.
    bool hasResultObj() const {
        return has(PlanStageSlots::kResult) && !_data->resultInfoEffects.has_value();
    }

    // Adds a requirement for a materialized result object, and clears any prior requirement for
    // ResultInfo this PlanStageReqs may have had. This method also clears any ResultInfo-related
    // state that may have been set.
    PlanStageReqs& setResultObj() {
        _data->trackedFieldSet.reset();
        _data->resultInfoEffects.reset();
        _data->slotNameSet.emplace(PlanStageSlots::kResult);
        return *this;
    }

    // Returns true if this PlanStageReqs requires a ResultInfo, otherwise returns false.
    bool hasResultInfo() const {
        return _data->resultInfoEffects.has_value();
    }

    // Returns a FieldSet containing all strings N where result field N is needed to satisfy this
    // PlanStageReqs's ResultInfo requirement. This method will tassert if hasResultInfo() is false.
    const FieldSet& getResultInfoTrackedFieldSet() const {
        tassert(8428004, "Expected ResultInfo to be set", hasResultInfo());
        return *_data->trackedFieldSet;
    }

    const FieldEffects& getResultInfoEffects() const {
        tassert(8323510, "Expected ResultInfo to be set", _data->resultInfoEffects.has_value());
        return *_data->resultInfoEffects;
    }

    // This method will add a ResultInfo requirement with the specified "tracked fields" set. This
    // method assumes 'hasResultObj()' is false and will tassert if 'hasResultObj()' is true.
    //
    // Note: If a PlanStageReqs requires a materialized result object and you want to change it to
    // have a ResultInfo requirement instead, call clearResult() first before calling this method.
    PlanStageReqs& setResultInfo(const FieldSet& trackedFieldSet, const FieldEffects& effects) {
        tassert(8428005, "Expected result object requirement to not be set", !hasResultObj());

        _data->trackedFieldSet.emplace(trackedFieldSet);

        _data->resultInfoEffects.emplace(effects);
        _data->resultInfoEffects->narrow(*_data->trackedFieldSet);

        _data->slotNameSet.emplace(PlanStageSlots::kResult);

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

    const PlanStageSlots::SlotNameSet& getSlotNameSet() const {
        return _data->slotNameSet;
    }

private:
    bool hasType(SlotType t) const {
        return std::any_of(_data->slotNameSet.begin(), _data->slotNameSet.end(), [t](auto& item) {
            return item.first == t;
        });
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

    PlanStageReqs& clearAllOfType(SlotType t) {
        absl::erase_if(_data->slotNameSet, [t](auto& item) { return item.first == t; });
        return *this;
    }

    friend class PlanStageSlots;

    std::unique_ptr<Data> _data;
};  // class PlanStageReqs

std::ostream& operator<<(std::ostream& os, const PlanStageReqs& reqs);

struct ProjectionPlan {
    PlanStageReqs childReqs;
    bool reqResultObj;
    bool reqResultInfo;
    bool isCoveredProjection;
    bool isInclusion;
    std::vector<std::string> paths;
    std::vector<ProjectNode> nodes;
    std::vector<std::string> nothingPaths;
    std::vector<std::string> updatedPaths;
    std::vector<std::string> resultPaths;
    StringMap<Expression*> pathExprMap;
    FieldEffects newEffects;
};

/**
 * A stage builder which builds an executable tree using slot-based PlanStages.
 */
class SlotBasedStageBuilder final : public StageBuilder<std::pair<SbStage, PlanStageData>> {
public:
    using PlanType = std::pair<SbStage, PlanStageData>;
    using BaseType = StageBuilder<PlanType>;

    static constexpr auto kMeta = PlanStageSlots::SlotType::kMeta;
    static constexpr auto kField = PlanStageSlots::SlotType::kField;
    static constexpr auto kSortKey = PlanStageSlots::SlotType::kSortKey;

    static constexpr auto kResult = PlanStageSlots::kResult;
    static constexpr auto kRecordId = PlanStageSlots::kRecordId;
    static constexpr auto kReturnKey = PlanStageSlots::kReturnKey;
    static constexpr auto kSnapshotId = PlanStageSlots::kSnapshotId;
    static constexpr auto kIndexIdent = PlanStageSlots::kIndexIdent;
    static constexpr auto kIndexKey = PlanStageSlots::kIndexKey;
    static constexpr auto kIndexKeyPattern = PlanStageSlots::kIndexKeyPattern;
    static constexpr auto kPrefetchedResult = PlanStageSlots::kPrefetchedResult;
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
     * descendants.
     *
     * This method is a wrapper around 'build(const QuerySolutionNode*, const PlanStageReqs&)'.
     */
    PlanType build(const QuerySolutionNode* root) final;

private:
    std::pair<SbStage, PlanStageSlots> buildTree();

    inline void analyzeTree() {
        _qsnAnalysis.analyzeTree(_root);
    }

    inline const QsnInfo& getQsnInfo(const QuerySolutionNode* qsNode) const {
        return _qsnAnalysis.getQsnInfo(qsNode);
    }
    inline const QsnInfo& getQsnInfo(const std::unique_ptr<QuerySolutionNode>& qsNode) const {
        return _qsnAnalysis.getQsnInfo(qsNode.get());
    }

    const FieldSet& getPostimageAllowedFields(const QuerySolutionNode* qsNode) const {
        return *getQsnInfo(qsNode).postimageAllowedFields;
    }
    const FieldSet& getPostimageAllowedFields(
        const std::unique_ptr<QuerySolutionNode>& qsNode) const {
        return getPostimageAllowedFields(qsNode.get());
    }
    std::vector<const FieldSet*> getPostimageAllowedFieldSets(
        const std::vector<const QuerySolutionNode*>& childNodes) const {
        // Build a vector of "const FieldSet" pointers to the children's allowed field sets
        // and return it.
        std::vector<const FieldSet*> allowedFieldSets;
        allowedFieldSets.reserve(childNodes.size());
        for (const auto& qsNode : childNodes) {
            allowedFieldSets.push_back(&getPostimageAllowedFields(qsNode));
        }

        return allowedFieldSets;
    }
    std::vector<const FieldSet*> getPostimageAllowedFieldSets(
        const std::vector<std::unique_ptr<QuerySolutionNode>>& childNodes) const {
        // Build a vector of "const FieldSet" pointers to the children's allowed field sets
        // and return it.
        std::vector<const FieldSet*> allowedFieldSets;
        allowedFieldSets.reserve(childNodes.size());
        for (const auto& nodePtr : childNodes) {
            allowedFieldSets.push_back(&getPostimageAllowedFields(nodePtr.get()));
        }
        return allowedFieldSets;
    }

    bool hasProjectionInfo(const QuerySolutionNode* qsNode) const {
        return _qsnAnalysis.hasProjectionInfo(qsNode);
    }
    bool hasProjectionInfo(const std::unique_ptr<QuerySolutionNode>& qsNode) const {
        return _qsnAnalysis.hasProjectionInfo(qsNode);
    }

    const ProjectionInfo& getProjectionInfo(const QuerySolutionNode* qsNode) const {
        return _qsnAnalysis.getProjectionInfo(qsNode);
    }
    const ProjectionInfo& getProjectionInfo(
        const std::unique_ptr<QuerySolutionNode>& qsNode) const {
        return _qsnAnalysis.getProjectionInfo(qsNode);
    }

    /**
     * This method will build an SBE PlanStage tree for QuerySolutionNode 'root' and its
     * descendants.
     *
     * Based on the type of 'root', this method will dispatch to the appropriate buildXXX()
     * method. This method will also handle generating calls to getField() to satisfy kField
     * reqs that were not satisfied by the buildXXX() method.
     */
    std::pair<SbStage, PlanStageSlots> build(const QuerySolutionNode* node,
                                             const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildCollScan(const QuerySolutionNode* root,
                                                     const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildVirtualScan(const QuerySolutionNode* root,
                                                        const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildIndexScan(const QuerySolutionNode* root,
                                                      const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildCountScan(const QuerySolutionNode* root,
                                                      const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildFetch(const QuerySolutionNode* root,
                                                  const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildLimit(const QuerySolutionNode* root,
                                                  const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildSkip(const QuerySolutionNode* root,
                                                 const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildSort(const QuerySolutionNode* root,
                                                 const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildSortCovered(const QuerySolutionNode* root,
                                                        const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildSortKeyGenerator(const QuerySolutionNode* root,
                                                             const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildSortMerge(const QuerySolutionNode* root,
                                                      const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildMatch(const QuerySolutionNode* root,
                                                  const PlanStageReqs& reqs);

    /**
     * Builds a complete $unwind stage, including extraction of the field to be unwound from the
     * source document, performing the unwind, and projecting the results to the output document.
     */
    std::pair<SbStage, PlanStageSlots> buildUnwind(const QuerySolutionNode* root,
                                                   const PlanStageReqs& reqs);
    /**
     * Enables an $LU stage to build the absorbed $unwind's unwinding and results projection only,
     * as the $lookup, which is conceptually the child of the $unwind, is built directly via a call
     * to buildEqLookupUnwind() with no parent call to buildUnwind() since the $unwind was erased
     * from the pipeline before the plan was finalized. Used for the special case of a nonexistent
     * foreign collection, where the $lookup result array is empty and thus its materialization is
     * not a performance or memory problem.
     */
    std::pair<SbStage, PlanStageSlots> buildOnlyUnwind(const UnwindNode::UnwindSpec& un,
                                                       const PlanStageReqs& reqs,
                                                       PlanNodeId nodeId,
                                                       SbStage& stage,
                                                       PlanStageSlots& outputs,
                                                       SbSlot childResultSlot,
                                                       SbSlot getFieldSlot);

    std::pair<SbStage, PlanStageSlots> buildReplaceRoot(const QuerySolutionNode* root,
                                                        const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildProjection(const QuerySolutionNode* root,
                                                       const PlanStageReqs& reqs);


    std::pair<SbStage, PlanStageSlots> buildExtractFieldPathsStage(const QuerySolutionNode* root,
                                                                   const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildOr(const QuerySolutionNode* root,
                                               const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildTextMatch(const QuerySolutionNode* root,
                                                      const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildReturnKey(const QuerySolutionNode* root,
                                                      const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildEof(const QuerySolutionNode* root,
                                                const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildAndHash(const QuerySolutionNode* root,
                                                    const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildAndSorted(const QuerySolutionNode* root,
                                                      const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildShardFilter(const QuerySolutionNode* root,
                                                        const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildSearch(const QuerySolutionNode* root,
                                                   const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildWindow(const QuerySolutionNode* root,
                                                   const PlanStageReqs& reqs);

    /**
     * Constructs an optimized SBE plan for 'root' in the case that the fields of the shard key
     * pattern are provided by the child index scan. In this case, the SBE plan for the child
     * index scan node will fill out slots for the necessary components of the index key. These
     * slots can be read directly in order to determine the shard key that should be passed to
     * the 'shardFiltererSlot'.
     */
    std::pair<SbStage, PlanStageSlots> buildShardFilterCovered(const QuerySolutionNode* root,
                                                               const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildGroup(const QuerySolutionNode* root,
                                                  const PlanStageReqs& reqs);

    std::tuple<SbStage, std::vector<std::string>, SbSlotVector, PlanStageSlots> buildGroupImpl(
        SbStage stage,
        const PlanStageReqs& reqs,
        PlanStageSlots childOutputs,
        const GroupNode* groupNode);

    std::tuple<SbStage, std::vector<std::string>, SbSlotVector, PlanStageSlots> buildGroupImplBlock(
        SbStage stage,
        const PlanStageReqs& reqs,
        const PlanStageSlots& childOutputs,
        const GroupNode* groupNode,
        bool& blockSucceeded);

    std::tuple<SbStage, std::vector<std::string>, SbSlotVector, PlanStageSlots>
    buildGroupImplScalar(SbStage stage,
                         const PlanStageReqs& reqs,
                         PlanStageSlots& childOutputs,
                         const GroupNode* groupNode);

    std::pair<SbStage, PlanStageSlots> buildEqLookup(const QuerySolutionNode* root,
                                                     const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildEqLookupUnwind(const QuerySolutionNode* root,
                                                           const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildNestedLoopJoinEmbeddingNode(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<SbStage, PlanStageSlots> buildUnpackTsBucket(const QuerySolutionNode* root,
                                                           const PlanStageReqs& reqs);

    /**
     * This struct is used to record a plan for how to produce a materialized result object.
     * 'ResultPlan' is used by getResultPlan() and makeResultUsingPlan().
     */
    struct ResultPlan {
        enum Type : unsigned int {
            kUseResultInfo,
            kUseFixedPlan,
        };

        ResultPlan(Type type, PlanStageReqs reqs) : type(type), reqs(std::move(reqs)) {}

        // Indicates whether this ResultPlan is a "ResultInfo"-based plan (kUseResultInfo)
        // or a "fixed" plan (kUseFixedPlan).
        Type type;

        // Provides the PlanStageReqs that should be passed in when build() is called on the
        // QSN node.
        PlanStageReqs reqs;

        // Provides a list of the kField reqs from the parent that can be satisfied by setting
        // the corresponding kField slots to "Nothing". 'nothingPaths' is used for both
        // "ResultInfo"-based plans (kUseResultInfo) or "fixed" plans (kUseFixedPlan).
        std::vector<std::string> nothingPaths;

        // Lists the names of the fields that are needed for a "fixed" plan. Note that
        // 'fixedPlanFields' is only used for "fixed" plans (kUseFixedPlan).
        std::vector<std::string> fixedPlanFields;
    };

    /**
     * Prior to building the SBE tree for a given QSN node, if 'reqs' has a result object req the
     * outer build() method will call this function to check if there is a way to remove the result
     * object req and replace it with either a ResultInfo req or individual kField reqs.
     *
     * If it's possible to replace the result object req with ResultInfo/kField reqs, then this
     * function will return a ResultPlan object that specifies how the reqs for the buildXXX()
     * method can be adjusted and what needs to be done to produce the result object after the
     * buildXXX() method returns. Otherwise, this function returns nullptr.
     */
    std::unique_ptr<ResultPlan> getResultPlan(const QuerySolutionNode* qsNode,
                                              const PlanStageReqs& reqs);

    /**
     * If the outer build() method called getResultPlan() and got back a non-null ResultPlan, then
     * the outer build() method will call this function after the buildXXX() method returns. This
     * function handle updating the SBE tree as appropriate to produce the result object (based on
     * the specified ResultPlan).
     */
    std::pair<SbStage, PlanStageSlots> makeResultUsingPlan(SbStage stage,
                                                           PlanStageSlots outputs,
                                                           const QuerySolutionNode* qsNode,
                                                           std::unique_ptr<ResultPlan> plan);

    MONGO_COMPILER_NOINLINE
    std::unique_ptr<ProjectionPlan> makeProjectionPlan(const QuerySolutionNode* root,
                                                       const PlanStageReqs& reqs);

    MONGO_COMPILER_NOINLINE
    std::pair<SbStage, PlanStageSlots> buildProjectionImpl(const QuerySolutionNode* root,
                                                           const PlanStageReqs& reqs,
                                                           std::unique_ptr<ProjectionPlan> plan,
                                                           SbStage stage,
                                                           PlanStageSlots outputs);

    /**
     * Given a scalar filter Expression it tries to produced the vectorised stage. If this is
     * possible it returns a pair {vectorisedStage, true}. If this is not possible it adds a
     * block-to-row transition and returns {newStage, false}.
     */
    std::pair<SbStage, bool> buildVectorizedFilterExpr(SbStage stage,
                                                       const PlanStageReqs& reqs,
                                                       SbExpr scalarFilterExpression,
                                                       PlanStageSlots& outputs,
                                                       PlanNodeId nodeId);

    SbExpr buildLimitSkipAmountExpression(LimitSkipParameterization canBeParameterized,
                                          long long amount,
                                          boost::optional<sbe::value::SlotId>& slot);
    SbExpr buildLimitSkipSumExpression(LimitSkipParameterization canBeParameterized,
                                       size_t limitSkipSum);

    /**
     * Returns a CollectionPtr corresponding to the collection that we are currently building a
     * plan over. If no current namespace is configured, a CollectionPtr referencing the main
     * collection tracked by '_collections' is returned.
     */
    CollectionPtr getCurrentCollection(const PlanStageReqs& reqs) const;

    std::pair<std::vector<std::string>, sbe::value::SlotVector> buildSearchMetadataSlots();

    sbe::value::SlotIdGenerator _slotIdGenerator;
    sbe::value::FrameIdGenerator _frameIdGenerator;
    sbe::value::SpoolIdGenerator _spoolIdGenerator;

    // Hash map tracking the InLists used by the SBE plan being built.
    absl::flat_hash_map<const InMatchExpression*, sbe::InList*> _inListsMap;

    // Hash set tracking the Collators used by the SBE plan being built.
    absl::flat_hash_map<const CollatorInterface*, const CollatorInterface*> _collatorsMap;

    // Maintains a mapping from AccumulationStatements / WindowFunctionStatements to their
    // corresponding SortSpecs (stored in slots).
    absl::flat_hash_map<const void*, sbe::value::SlotId> _sortSpecMap;

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

    QsnAnalysis _qsnAnalysis;

    bool _buildHasStarted{false};

    // Common parameters to SBE stage builder functions.
    StageBuilderState _state;
};  // class SlotBasedStageBuilder

SbStage buildBlockToRow(SbStage stage, StageBuilderState& state, PlanStageSlots& outputs);

std::pair<SbStage, SbSlotVector> buildBlockToRow(SbStage stage,
                                                 StageBuilderState& state,
                                                 PlanStageSlots& outputs,
                                                 SbSlotVector individualSlots);
}  // namespace mongo::stage_builder
