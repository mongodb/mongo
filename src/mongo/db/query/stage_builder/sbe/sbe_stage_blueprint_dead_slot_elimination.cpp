// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint_dead_slot_elimination.h"

#include <variant>
#include <vector>

namespace mongo::stage_builder {
namespace {
/**
 * Removes dead project entries, scan fields, and scalar group accumulators from the plan.
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
        return std::visit([&](auto& concrete) { return applyTo(concrete, node, ctx); }, *node);
    }

private:
    static void removeUse(sbe::value::SlotId slot,
                          const SbDetachedNode& node,
                          SbBlueprintRewriteContext& ctx) {
        auto& usage = ctx.slotUsage();
        if (usage.removeUse(slot) == 0) {
            auto definer = usage.getDefiningNode(slot);
            if (definer && *definer != node.get()) {
                ctx.enqueue(*definer);
            }
        }
    }

    template <typename T>
    static boost::optional<SbDetachedNode> applyTo(T&,
                                                   SbDetachedNode&,
                                                   SbBlueprintRewriteContext&) {
        return boost::none;
    }

    static boost::optional<SbDetachedNode> applyTo(SbBlueprintProject& project,
                                                   SbDetachedNode& node,
                                                   SbBlueprintRewriteContext& ctx) {
        auto& usage = ctx.slotUsage();
        auto& projects = project.projects;
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
                removeUse(slot.getId(), node, ctx);
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

    static boost::optional<SbDetachedNode> applyTo(SbBlueprintScan& scan,
                                                   SbDetachedNode& node,
                                                   SbBlueprintRewriteContext& ctx) {
        // A field slot reads no other slot, so removing one never leaves another slot dead.
        auto& usage = ctx.slotUsage();
        auto& names = scan.info->scanFieldNames;
        auto& slots = scan.info->scanFieldSlots;
        size_t keep = 0;
        for (size_t i = 0; i < slots.size(); ++i) {
            if (usage.numUses(slots[i]) == 0) {
                usage.removeDef(slots[i]);
                continue;
            }
            if (keep != i) {
                names[keep] = std::move(names[i]);
                slots[keep] = slots[i];
            }
            ++keep;
        }
        if (keep == slots.size()) {
            return boost::none;
        }
        names.resize(keep);
        slots.resize(keep);
        return std::move(node);
    }

    static boost::optional<SbDetachedNode> removeDeadAccumulators(
        std::vector<SbBlueprintHashAggAccumulator>& accumulators,
        bool definesSpillSlots,
        SbDetachedNode& node,
        SbBlueprintRewriteContext& ctx) {
        auto& usage = ctx.slotUsage();
        const auto removed = std::erase_if(accumulators, [&](const auto& acc) {
            if (usage.numUses(acc.outSlot) != 0) {
                return false;
            }

            // Spill slots are internal to the group. Their uses in merge expressions do not
            // keep an otherwise unread accumulator alive, but must be removed from the summary.
            SbSlotCollector uses;
            acc.use(uses);
            for (auto slot : uses.slots()) {
                removeUse(slot, node, ctx);
            }
            usage.removeDef(acc.outSlot);
            if (definesSpillSlots) {
                usage.removeDef(acc.spillSlot);
            }
            return true;
        });
        if (removed == 0) {
            return boost::none;
        }
        // Keep all grouping and cluster keys, even when unread above the group: they determine
        // its output cardinality. A group with no accumulators must not be replaced by its child.
        return std::move(node);
    }

    static boost::optional<SbDetachedNode> applyTo(SbBlueprintHashAgg& group,
                                                   SbDetachedNode& node,
                                                   SbBlueprintRewriteContext& ctx) {
        return removeDeadAccumulators(group.accumulators, !group.compact, node, ctx);
    }

    static boost::optional<SbDetachedNode> applyTo(SbBlueprintStreamingAgg& group,
                                                   SbDetachedNode& node,
                                                   SbBlueprintRewriteContext& ctx) {
        return removeDeadAccumulators(group.accumulators, true /* definesSpillSlots */, node, ctx);
    }
};
}  // namespace

std::unique_ptr<SbBlueprintRewriteRule> makeDeadSlotEliminationRule() {
    return std::make_unique<DeadSlotEliminationRule>();
}
}  // namespace mongo::stage_builder
