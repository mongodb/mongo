// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint.h"
#include "mongo/db/query/stage_builder/sbe/type_signature.h"

#include <cstddef>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::stage_builder {
/**
 * Tracks uses and definitions of slots in a blueprint tree.
 *
 * For example, given the tree
 *
 *     [2] project   defines s3, uses s1 and s2
 *       |
 *     [1] filter    uses s1
 *       |
 *     [0] scan      defines s1 and s2
 *
 * and a root consumer that reads s3, build() records:
 *
 *     slot            s1    s2    s3
 *     defining node   [0]   [0]   [2]
 *     num uses        2     1     1 (the root consumer)
 *
 * A slot read but defined by no node comes from the RuntimeEnvironment.
 *
 * It also records the types each defined slot's values may have: a scan or fetch record is always
 * an object, and a project entry's values have the type signature of its expression.
 */
class SbBlueprintSlotUsage {
public:
    /**
     * What the summary records about one slot.
     */
    struct SlotInfo {
        // The node that defines the slot, or none if no node in the tree defines it.
        boost::optional<SbBlueprintNodeIdx> definingNode;

        // How many times the slot is read in the tree, counting the consumer of the root.
        size_t numUses{0};

        // The types the slot's values may have at its definer. Any type, if nothing is known.
        // BlockHashAgg re-exposes its group-by slots as blocks, so these facts do not describe
        // their values above that stage.
        TypeSignature typeSig = TypeSignature::kAnyType;
    };

    /**
     * Summarizes the tree rooted at 'root'. 'rootUses' lists the slots read by the consumer of
     * the root node (the PlanExecutor); each counts as one use.
     */
    static SbBlueprintSlotUsage build(SbBlueprint& blueprint,
                                      SbBlueprintNodeIdx root,
                                      const sbe::value::SlotVector& rootUses);

    /**
     * Returns the node that defines 'slot', or none if no node in the tree defines it.
     */
    boost::optional<SbBlueprintNodeIdx> getDefiningNode(sbe::value::SlotId slot) const;

    /**
     * Returns how many times 'slot' is read in the tree, counting the consumer of the root.
     */
    size_t numUses(sbe::value::SlotId slot) const;

    /**
     * Records that one read of 'slot' was removed from the tree. Returns how many reads are left.
     */
    size_t removeUse(sbe::value::SlotId slot);

    /**
     * Records that 'slot' is no longer defined by any node in the tree and clears its type facts.
     */
    void removeDef(sbe::value::SlotId slot);

    /**
     * Whether both summaries record the same defining nodes, use counts and type signatures.
     */
    bool operator==(const SbBlueprintSlotUsage& other) const;

    /**
     * Returns what the summary records about 'slot'. The slot index must be in bounds.
     */
    const SlotInfo& getInfo(sbe::value::SlotId slot) const;

    /**
     * Returns what the summary records about 'slot', for a rule to update. The slot index must
     * be in bounds.
     */
    SlotInfo& getInfo(sbe::value::SlotId slot);

    /**
     * Calls 'fn' for each slot that is read in the tree but defined by no node in it.
     */
    template <typename F>
    void forEachUndefinedUse(F&& fn) const {
        for (size_t slot = 0; slot < _slots.size(); ++slot) {
            if (_slots[slot].numUses > 0 && !_slots[slot].definingNode) {
                fn(static_cast<sbe::value::SlotId>(slot));
            }
        }
    }

private:
    // Indexed by slot id.
    std::vector<SlotInfo> _slots;
};
}  // namespace mongo::stage_builder
