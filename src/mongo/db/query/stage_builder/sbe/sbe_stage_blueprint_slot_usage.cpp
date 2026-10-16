// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint_slot_usage.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <variant>

namespace mongo::stage_builder {
namespace {
size_t slotIndex(sbe::value::SlotId slot) {
    tassert(13627002, "Negative slot id", slot >= 0);
    return static_cast<size_t>(slot);
}

template <typename T>
void growTo(std::vector<T>& v, size_t idx) {
    if (v.size() <= idx) {
        v.resize(idx + 1);
    }
}
}  // namespace

SbBlueprintSlotUsage SbBlueprintSlotUsage::build(SbBlueprint& blueprint,
                                                 SbBlueprintNodeIdx root,
                                                 const sbe::value::SlotVector& rootUses) {
    SbBlueprintSlotUsage usage;
    auto addUses = [&](const sbe::value::SlotVector& slots) {
        for (auto slot : slots) {
            const size_t idx = slotIndex(slot);
            growTo(usage._numUses, idx);
            ++usage._numUses[idx];
        }
    };
    addUses(rootUses);

    std::vector<SbBlueprintNodeIdx> toVisit{root};
    while (!toVisit.empty()) {
        const SbBlueprintNodeIdx node = toVisit.back();
        toVisit.pop_back();
        SbBlueprintNode& n = blueprint[node];

        SbSlotCollector defs;
        SbSlotCollector uses;
        std::visit(
            [&](auto& concrete) {
                concrete.def(defs);
                concrete.use(uses);
                concrete.visitChildren([&](SbBlueprintNodeIdx child) { toVisit.push_back(child); });
            },
            n);

        for (auto slot : defs.slots()) {
            const size_t idx = slotIndex(slot);
            growTo(usage._definingNode, idx);
            tassert(13627003,
                    str::stream() << "Slot " << slot
                                  << " is defined by more than one blueprint node",
                    !usage._definingNode[idx]);
            usage._definingNode[idx] = node;
        }
        addUses(uses.slots());
    }

    // Every used slot has an entry in '_definingNode'.
    if (usage._definingNode.size() < usage._numUses.size()) {
        usage._definingNode.resize(usage._numUses.size());
    }
    return usage;
}

boost::optional<SbBlueprintNodeIdx> SbBlueprintSlotUsage::getDefiningNode(
    sbe::value::SlotId slot) const {
    const size_t idx = slotIndex(slot);
    return idx < _definingNode.size() ? _definingNode[idx] : boost::none;
}

size_t SbBlueprintSlotUsage::numUses(sbe::value::SlotId slot) const {
    const size_t idx = slotIndex(slot);
    return idx < _numUses.size() ? _numUses[idx] : 0;
}
}  // namespace mongo::stage_builder
