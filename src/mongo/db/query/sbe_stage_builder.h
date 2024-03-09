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

    struct Data {
        // Slot type-and-name to SlotId map for the output slots produced by this plan stage.
        SlotNameMap slotNameToIdMap;

        // If this PlanStageSlots has a ResultInfo then 'resultInfoChanges' will hold the recorded
        // changes for the ResultInfo, otherwise it will be set to boost::none.
        boost::optional<ProjectionEffects> resultInfoChanges;
    };

    static std::unique_ptr<Data> cloneData(const std::unique_ptr<Data>& other) {
        if (other) {
            return std::make_unique<Data>(*other);
        }
        return {};
    }

    /**
     * When the build() depth-first traversal backtracks through a merge point in the QSN tree,
     * this method handles doing the "merge" process.
     */
    static PlanStageSlots makeMergedPlanStageSlots(StageBuilderState& state,
                                                   PlanNodeId nodeId,
                                                   const PlanStageReqs& reqs,
                                                   std::vector<PlanStageTree>& stagesAndSlots);

    /**
     * This is a helper function used by makeMergedPlanStageSlots() is called that handles the
     * case where one or more of the PlanStageOutputs objects have a ResultInfo.
     */
    static void mergeResultInfos(StageBuilderState& state,
                                 PlanNodeId nodeId,
                                 std::vector<PlanStageTree>& trees);

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

    // Returns true if this PlanStageSlots has a mapping for 'name'.
    bool has(const UnownedSlotName& name) const {
        return _data->slotNameToIdMap.count(name);
    }

    // Returns the slot corresponding to 'name'. This method will raise a tassert if
    // this PlanStageSlots does not have a mapping for 'name'.
    TypedSlot get(const UnownedSlotName& name) const {
        auto it = _data->slotNameToIdMap.find(name);
        invariant(it != _data->slotNameToIdMap.end());
        return it->second;
    }

    // Returns the slot corresponding to 'name' if this PlanStageSlot has a mapping for 'name',
    // otherwise returns boost::none.
    boost::optional<TypedSlot> getIfExists(const UnownedSlotName& name) const {
        if (auto it = _data->slotNameToIdMap.find(name); it != _data->slotNameToIdMap.end()) {
            return it->second;
        }
        return boost::none;
    }

    // This method is like getIfExists(), except that it returns 'boost::optional<SlotId>'
    // instead of 'boost::optional<TypedSlot>'.
    boost::optional<sbe::value::SlotId> getSlotIfExists(const UnownedSlotName& name) const {
        if (auto it = _data->slotNameToIdMap.find(name); it != _data->slotNameToIdMap.end()) {
            return it->second.slotId;
        }
        return boost::none;
    }

    // Maps 'name' to 'slot' and clears any prior mapping the 'name' may have had.
    void set(const UnownedSlotName& name, TypedSlot slot) {
        _data->slotNameToIdMap.insert_or_assign(name, slot);
    }
    void set(OwnedSlotName name, TypedSlot slot) {
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
        for (auto it = _data->slotNameToIdMap.begin(); it != _data->slotNameToIdMap.end();) {
            if (it->first.first == kField) {
                _data->slotNameToIdMap.erase(it++);
                continue;
            }
            ++it;
        }
    }

    // If 'path' is a dotted path, this method will call 'clearField(path)' and then it will call
    // clearField() on every prefix of 'path'. If 'path' is not a dotted path, this method will
    // just call 'clearField(path)' and return.
    void clearFieldAndAllPrefixes(StringData path) {
        for (;;) {
            clearField(path);

            size_t pos = path.rfind('.');
            if (pos == std::string::npos) {
                break;
            }

            path = path.substr(0, pos);
        }
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
     * a materialized result object by taking the contents of the ResultInfo base object as a
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
    TypedSlot getResultObj() const {
        tassert(8428000, "Expected result object to be set", hasResultObj());

        auto it = _data->slotNameToIdMap.find(kResult);
        invariant(it != _data->slotNameToIdMap.end());

        return it->second;
    }

    // If 'hasResultObj()' is true this method returns the slot that holds the materialized result
    // object, otherwise it will return boost::none.
    boost::optional<TypedSlot> getResultObjIfExists() const {
        if (hasResultObj()) {
            return getResultObj();
        }
        return boost::none;
    }

    // This method is like getResultObjIfExists(), except that it returns 'boost::optional<SlotId>'
    // instead of 'boost::optional<TypedSlot>'.
    boost::optional<sbe::value::SlotId> getResultObjSlotIfExists() const {
        if (hasResultObj()) {
            return getResultObj().slotId;
        }
        return boost::none;
    }

    // Maps kResult to 'slot', designates kResult as being a "materialized result object", and
    // clears any prior mapping or designation that kResult may have had. setResultObj() also
    // clears any ResultInfo-related state that may have been set.
    void setResultObj(TypedSlot slot) {
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
    TypedSlot getResultInfoBaseObj() const {
        tassert(8428001, "Expected ResultInfo to be set", hasResultInfo());

        auto it = _data->slotNameToIdMap.find(kResult);
        invariant(it != _data->slotNameToIdMap.end());

        return it->second;
    }

    // If 'hasResultInfo()' is true this method returns the slot that holds the ResultInfo base
    // object, otherwise it will return boost::none.
    boost::optional<TypedSlot> getResultInfoBaseObjIfExists() const {
        if (hasResultInfo()) {
            return getResultInfoBaseObj();
        }
        return boost::none;
    }

    // Maps kResult to 'slot', designates kResult as being as a "ResultInfo base object", clears
    // any prior mapping or designation that kResult may have had, and sets 'resultInfoChanges' to
    // hold an empty list of changed fields.
    void setResultInfoBaseObj(TypedSlot slot) {
        set(kResult, slot);
        _data->resultInfoChanges.emplace();
    }

    // Returns the list of changed fields stored in 'resultInfoChanges'. This method will tassert
    // if 'hasResultInfo()' is false.
    const ProjectionEffects& getResultInfoChanges() const {
        tassert(8428002, "Expected ResultInfo to be set", hasResultInfo());

        return *_data->resultInfoChanges;
    }

    // Add the fields specified in 'drops' and 'modifys' to the list of changed fields stored
    // in 'resultInfoChanges'. This method will tassert if 'hasResultInfo()' is false.
    void addResultInfoChanges(const std::vector<std::string>& drops,
                              const std::vector<std::string>& modifys) {
        tassert(8428003, "Expected ResultInfo to be set", hasResultInfo());

        auto& changes = *_data->resultInfoChanges;
        changes = ProjectionEffects(FieldSet::makeOpenSet(drops), modifys, {}).compose(changes);
    }

    // Returns a sorted list of all the names in this PlanStageSlots has that are required by
    // 'reqs', plus any additional names needed by 'reqs' that this PlanStageSlots does not satisfy.
    std::vector<OwnedSlotName> getRequiredNamesInOrder(const PlanStageReqs& reqs) const;

    // Returns a list of slots that correspond pairwise to the list of names produced by calling
    // 'getRequiredNamesInOrder(reqs)'.
    TypedSlotVector getRequiredSlotsInOrder(const PlanStageReqs& reqs) const;

    // This method returns a de-dupped list of all the slots that correspond to the names produced
    // calling by 'getRequiredNamesInOrder(reqs)'. The list returned by this method is sorted by
    // slot ID.
    TypedSlotVector getRequiredSlotsUnique(const PlanStageReqs& reqs) const;

    // Returns a sorted list of all the name->slot mappings in this PlanStageSlots, sorted by
    // slot name.
    std::vector<std::pair<UnownedSlotName, TypedSlot>> getAllNameSlotPairsInOrder() const;

    // This method calls getAllNameSlotPairsInOrder() and then returns a list of the slots only,
    // sorted by slot name.
    TypedSlotVector getAllSlotsInOrder() const;

    // This method computes the list of required names 'L = getRequiredNamesInOrder(reqs)', and
    // then it adds a mapping 'N->NothingSlot' for each name N in list L where 'has(N)' is false.
    void setMissingRequiredNamedSlotsToNothing(StageBuilderState& state, const PlanStageReqs& reqs);

    // Removes all names (and their corresponding mappings) from this PlanStageSlots that do not
    // appear in list produced by calling 'getRequiredNamesInOrder(reqs)'.
    void clearNonRequiredSlots(const PlanStageReqs& reqs, bool saveResultObj = true);

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

    struct Data {
        // The set of the type-and-names of the slots required as inputs by this plan stage.
        PlanStageSlots::SlotNameSet slotNameSet;

        boost::optional<FieldSet> allowedSet;

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

    // If 'path' is a dotted path, this method will call 'clearField(path)' and then it will call
    // clearField() on every prefix of 'path'. If 'path' is not a dotted path, this method will
    // just call 'clearField(path)' and return.
    PlanStageReqs& clearFieldAndAllPrefixes(StringData path) {
        for (;;) {
            clearField(path);

            size_t pos = path.rfind('.');
            if (pos == std::string::npos) {
                break;
            }

            path = path.substr(0, pos);
        }

        return *this;
    }

    // Returns true if this PlanStageReqs has an explicit requirement for a materialized result
    // object or a ResultInfo. To distinguish between these two cases, use the hasResultObj() or
    // hasResultInfo() methods.
    bool hasResult() const {
        return has(PlanStageSlots::kResult) || _data->allowedSet.has_value();
    }

    // This method clears any result object or ResultInfo requirements that this PlanStageReqs may
    // have had, and it also clears any ResultInfo-related state that may have been set. After this
    // method returns, hasResult() and hasResultObject() and hasResultInfo() will all be false.
    PlanStageReqs& clearResult() {
        _data->allowedSet.reset();
        clear(PlanStageSlots::kResult);
        return *this;
    }

    // Returns true if this PlanStageReqs requires a materialized result object, otherwise returns
    // false.
    bool hasResultObj() const {
        return has(PlanStageSlots::kResult) && !_data->allowedSet.has_value();
    }

    // Adds a requirement for a materialized result object, and clears any prior requirement for
    // ResultInfo this PlanStageReqs may have had. This method also clears any ResultInfo-related
    // state that may have been set.
    PlanStageReqs& setResultObj() {
        _data->allowedSet.reset();
        _data->slotNameSet.emplace(PlanStageSlots::kResult);
        return *this;
    }

    // Returns true if this PlanStageReqs requires a ResultInfo, otherwise returns false.
    bool hasResultInfo() const {
        return _data->allowedSet.has_value();
    }

    // Returns a FieldSet containing all strings N where result field N is needed to satisfy this
    // PlanStageReqs's ResultInfo requirement. This method will tassert if hasResultInfo() is false.
    const FieldSet& getResultInfoAllowedSet() const {
        tassert(8428004, "Expected ResultInfo to be set", hasResultInfo());
        return *_data->allowedSet;
    }

    // This method will add a ResultInfo requirement with the specified "allowed" set. This method
    // assumes 'hasResultObj()' is false and will tassert if 'hasResultObj()' is true.
    //
    // Note: If a PlanStageReqs requires a materialized result object and you want to change it to
    // have a ResultInfo requirement instead, call clearResult() first before calling this method.
    PlanStageReqs& setResultInfo(const FieldSet& allowedSet) {
        tassert(8428005, "Expected result object requirement to not be set", !hasResultObj());

        _data->allowedSet.emplace(allowedSet);
        _data->slotNameSet.emplace(PlanStageSlots::kResult);

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
    bool reqResultInfo;
    bool produceResultObj;
    bool isInclusion;
    std::vector<std::string> paths;
    std::vector<ProjectNode> nodes;
    std::vector<std::string> nothingPaths;
    std::vector<std::string> resultPaths;
    std::vector<std::string> updatedPaths;
    StringMap<Expression*> updatedPathsExprMap;
    std::vector<std::string> resultInfoDrops;
    std::vector<std::string> resultInfoModifys;
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
     * descendants.
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
     * descendants.
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

    /**
     * Builds a complete $unwind stage, including extraction of the field to be unwound from the
     * source document, performing the unwind, and projecting the results to the output document.
     */
    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildUnwind(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);
    /**
     * Enables an $LU stage to build the absorbed $unwind's unwinding and results projection only,
     * as the $lookup, which is conceptually the child of the $unwind, is built directly via a call
     * to buildEqLookupUnwind() with no parent call to buildUnwind() since the $unwind was erased
     * from the pipeline before the plan was finalized. Used for the special case of a nonexistent
     * foreign collection, where the $lookup result array is empty and thus its materialization is
     * not a performance or memory problem.
     */
    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildOnlyUnwind(
        const UnwindNode* un,
        const PlanStageReqs& reqs,
        std::unique_ptr<sbe::PlanStage>& stage,
        PlanStageSlots& outputs,
        sbe::value::SlotId childResultSlot,
        sbe::value::SlotId getFieldSlot);

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

    std::tuple<SbStage, std::vector<std::string>, SbSlotVector, PlanStageSlots> buildGroupImpl(
        std::unique_ptr<sbe::PlanStage> stage,
        const PlanStageReqs& reqs,
        PlanStageSlots childOutputs,
        const GroupNode* groupNode);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildEqLookup(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildEqLookupUnwind(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildUnpackTsBucket(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    MONGO_COMPILER_NOINLINE
    std::unique_ptr<BuildProjectionPlan> makeBuildProjectionPlan(const QuerySolutionNode* root,
                                                                 const PlanStageReqs& reqs);

    MONGO_COMPILER_NOINLINE
    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildProjectionImpl(
        const QuerySolutionNode* root,
        const PlanStageReqs& reqs,
        std::unique_ptr<BuildProjectionPlan> plan,
        std::unique_ptr<sbe::PlanStage> stage,
        PlanStageSlots outputs);

    /**
     * Given a scalar filter Expression it tries to produced the vectorised stage. If this is
     * possible it returns a pair {vectorisedStage, true}. If this is not possible it adds a
     * block-to-row transition and returns {newStage, false}.
     */
    std::pair<std::unique_ptr<sbe::PlanStage>, bool> buildVectorizedFilterExpr(
        std::unique_ptr<sbe::PlanStage> stage,
        const PlanStageReqs& reqs,
        SbExpr scalarFilterExpression,
        PlanStageSlots& outputs,
        PlanNodeId nodeId);

    std::unique_ptr<sbe::EExpression> buildLimitSkipAmountExpression(
        LimitSkipParameterization canBeParameterized,
        long long amount,
        boost::optional<sbe::value::SlotId>& slot);
    std::unique_ptr<sbe::EExpression> buildLimitSkipSumExpression(
        LimitSkipParameterization canBeParameterized, size_t limitSkipSum);

    /**
     * Returns a CollectionPtr corresponding to the collection that we are currently building a
     * plan over. If no current namespace is configured, a CollectionPtr referencing the main
     * collection tracked by '_collections' is returned.
     */
    const CollectionPtr& getCurrentCollection(const PlanStageReqs& reqs) const;

    std::pair<std::vector<std::string>, sbe::value::SlotVector> buildSearchMetadataSlots();

    sbe::value::SlotIdGenerator _slotIdGenerator;
    sbe::value::FrameIdGenerator _frameIdGenerator;
    sbe::value::SpoolIdGenerator _spoolIdGenerator;

    // Hash set tracking the InListDatas used by the SBE plan being built.
    absl::flat_hash_set<InListData*> _inListsSet;

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
    absl::node_hash_map<const QuerySolutionNode*, QsnAnalysis> _analysis;

    bool _buildHasStarted{false};

    // Common parameters to SBE stage builder functions.
    StageBuilderState _state;
};  // class SlotBasedStageBuilder

std::unique_ptr<sbe::PlanStage> buildBlockToRow(std::unique_ptr<sbe::PlanStage> stage,
                                                StageBuilderState& state,
                                                PlanStageSlots& outputs);

std::pair<std::unique_ptr<sbe::PlanStage>, TypedSlotVector> buildBlockToRow(
    std::unique_ptr<sbe::PlanStage> stage,
    StageBuilderState& state,
    PlanStageSlots& outputs,
    TypedSlotVector individualSlots);
}  // namespace mongo::stage_builder
