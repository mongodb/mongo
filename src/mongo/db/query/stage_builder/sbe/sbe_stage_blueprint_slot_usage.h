// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint.h"

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
 */
class SbBlueprintSlotUsage {
public:
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
     * Calls 'fn' for each slot that is read in the tree but defined by no node in it.
     */
    template <typename F>
    void forEachUndefinedUse(F&& fn) const {
        for (size_t slot = 0; slot < _numUses.size(); ++slot) {
            if (_numUses[slot] > 0 && !_definingNode[slot]) {
                fn(static_cast<sbe::value::SlotId>(slot));
            }
        }
    }

private:
    // Both indexed by slot id.
    std::vector<boost::optional<SbBlueprintNodeIdx>> _definingNode;
    std::vector<size_t> _numUses;
};
}  // namespace mongo::stage_builder
