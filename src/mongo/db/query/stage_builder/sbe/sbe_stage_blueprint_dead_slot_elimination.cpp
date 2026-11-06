// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint_dead_slot_elimination.h"

namespace mongo::stage_builder {
namespace {
/**
 * Removes dead slots from the plan. Initially limited to only 'project' stages.
 */
class DeadSlotEliminationRule final : public SbBlueprintRewriteRule {
public:
    std::string_view name() const override {
        return "deadSlotElimination";
    }

    size_t depth() const override {
        return 1;
    }

    boost::optional<SbDetachedNode> apply(SbDetachedNode& node,
                                          SbBlueprintRewriteContext& ctx) override {
        auto* project = std::get_if<SbBlueprintProject>(&*node);
        if (!project) {
            return boost::none;
        }

        auto& usage = ctx.slotUsage();
        auto& projects = project->projects;
        // Not all slots are safe to remove. For instance, a slot may be computed simply to throw an
        // error. It's the caller's responsibility to ensure that these slots are not removed with
        // the SbBlueprint::keepSlot() interface.
        //
        // Projects with sequential binding may read defined slots left-to-right. Here, we work
        // right-to-left within a single project stage to remove potentially several cascading dead
        // slots in a single pass.
        size_t keep = projects.size();
        for (size_t i = projects.size(); i-- > 0;) {
            const auto& [expr, outSlot] = projects[i];
            if (usage.numUses(outSlot.getId()) != 0) {
                // Keep this slot.
                if (--keep != i) {
                    projects[keep] = std::move(projects[i]);
                }
                continue;
            }
            expr.visitReferencedSlots([&](SbSlot slot) {
                // Removing 'outSlot' makes 'slot' dead, queue the stage that defines 'slot' for
                // dead-slot-removal.
                if (usage.removeUse(slot.getId()) == 0) {
                    auto definer = usage.getDefiningNode(slot.getId());
                    if (definer && *definer != node.get()) {
                        ctx.enqueue(*definer);
                    }
                }
            });
            usage.removeDef(outSlot.getId());
        }
        const size_t removed = keep;
        // These slots are dead; remove them.
        projects.erase(projects.begin(), projects.begin() + keep);

        if (projects.empty()) {
            return node.takeChild(0);
        }
        if (removed == 0) {
            return boost::none;
        }
        return std::move(node);
    }
};
}  // namespace

std::unique_ptr<SbBlueprintRewriteRule> makeDeadSlotEliminationRule() {
    return std::make_unique<DeadSlotEliminationRule>();
}
}  // namespace mongo::stage_builder
