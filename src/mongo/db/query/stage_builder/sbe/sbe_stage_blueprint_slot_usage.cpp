// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint_slot_usage.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <type_traits>
#include <utility>
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

/**
 * Calls 'fn(slot, typeSig)' for each slot 'node' defines whose types are known.
 */
template <typename F>
void forEachDefinedSlotType(const SbBlueprintNode& node, F&& fn) {
    std::visit(
        [&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, SbBlueprintScan> ||
                          std::is_same_v<T, SbBlueprintMultiRangeScan> ||
                          std::is_same_v<T, SbBlueprintFetch>) {
                fn(n.info->recordSlot, TypeSignature::kObjectType);
            } else if constexpr (std::is_same_v<T, SbBlueprintProject>) {
                for (const auto& [expr, slot] : n.projects) {
                    if (auto typeSig = expr.getTypeSignature()) {
                        fn(slot.getId(), *typeSig);
                    }
                }
            }
        },
        node);
}
}  // namespace

SbBlueprintSlotUsage SbBlueprintSlotUsage::build(SbBlueprint& blueprint,
                                                 SbBlueprintNodeIdx root,
                                                 const sbe::value::SlotVector& rootUses) {
    SbBlueprintSlotUsage usage;
    auto addUses = [&](const sbe::value::SlotVector& slots) {
        for (auto slot : slots) {
            const size_t idx = slotIndex(slot);
            growTo(usage._slots, idx);
            ++usage._slots[idx].numUses;
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
            growTo(usage._slots, idx);
            auto& info = usage._slots[idx];
            tassert(13627003,
                    str::stream() << "Slot " << slot
                                  << " is defined by more than one blueprint node",
                    !info.definingNode);
            info.definingNode = node;
        }
        forEachDefinedSlotType(n, [&](sbe::value::SlotId slot, TypeSignature typeSig) {
            usage.getInfo(slot).typeSig = typeSig;
        });
        addUses(uses.slots());
    }
    return usage;
}

const SbBlueprintSlotUsage::SlotInfo& SbBlueprintSlotUsage::getInfo(sbe::value::SlotId slot) const {
    const size_t idx = slotIndex(slot);
    tassert(
        13627022, "Slot index is out of bounds in the blueprint slot summary", idx < _slots.size());
    return _slots[idx];
}

SbBlueprintSlotUsage::SlotInfo& SbBlueprintSlotUsage::getInfo(sbe::value::SlotId slot) {
    return const_cast<SlotInfo&>(std::as_const(*this).getInfo(slot));
}

boost::optional<SbBlueprintNodeIdx> SbBlueprintSlotUsage::getDefiningNode(
    sbe::value::SlotId slot) const {
    const size_t idx = slotIndex(slot);
    return idx < _slots.size() ? _slots[idx].definingNode : boost::none;
}

size_t SbBlueprintSlotUsage::numUses(sbe::value::SlotId slot) const {
    const size_t idx = slotIndex(slot);
    return idx < _slots.size() ? _slots[idx].numUses : 0;
}

size_t SbBlueprintSlotUsage::removeUse(sbe::value::SlotId slot) {
    auto& info = getInfo(slot);
    tassert(13627009, "Removed a use of a slot that has none", info.numUses > 0);
    return --info.numUses;
}

void SbBlueprintSlotUsage::removeDef(sbe::value::SlotId slot) {
    const size_t idx = slotIndex(slot);
    if (idx < _slots.size()) {
        auto& info = _slots[idx];
        info.definingNode = boost::none;
        info.typeSig = TypeSignature::kAnyType;
    }
}

bool SbBlueprintSlotUsage::operator==(const SbBlueprintSlotUsage& other) const {
    const SlotInfo empty;
    const size_t numSlots = std::max(_slots.size(), other._slots.size());
    for (size_t idx = 0; idx < numSlots; ++idx) {
        const auto& info = idx < _slots.size() ? _slots[idx] : empty;
        const auto& otherInfo = idx < other._slots.size() ? other._slots[idx] : empty;
        if (info.numUses != otherInfo.numUses || info.definingNode != otherInfo.definingNode ||
            info.typeSig != otherInfo.typeSig) {
            return false;
        }
    }
    return true;
}
}  // namespace mongo::stage_builder
