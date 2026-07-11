// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expressions/runtime_environment.h"

#include "mongo/bson/util/builder.h"

#include <iosfwd>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {
RuntimeEnvironment::RuntimeEnvironment(const RuntimeEnvironment& other)
    : _state{other._state}, _isSmp{other._isSmp} {
    for (auto&& [slotId, index] : _state->slots) {
        emplaceAccessor(slotId, index);
    }
}

void RuntimeEnvironment::registerSlot(value::TypeTags tag,
                                      value::Value val,
                                      bool owned,
                                      value::SlotId slotId) {
    emplaceAccessor(slotId, _state->pushSlot(slotId));
    _accessors.at(slotId).reset(value::TagValueMaybeOwned::fromRaw(owned, tag, val));
}

value::SlotId RuntimeEnvironment::registerSlot(std::string_view name,
                                               value::TypeTags tag,
                                               value::Value val,
                                               bool owned,
                                               value::SlotIdGenerator* slotIdGenerator) {
    auto slot = registerSlot(tag, val, owned, slotIdGenerator);
    _state->nameSlot(name, slot);
    return slot;
}

value::SlotId RuntimeEnvironment::registerSlot(value::TypeTags tag,
                                               value::Value val,
                                               bool owned,
                                               value::SlotIdGenerator* slotIdGenerator) {
    tassert(5645903, "Slot Id generator is null", slotIdGenerator);
    auto slot = slotIdGenerator->generate();
    registerSlot(tag, val, owned, slot);
    return slot;
}

value::SlotId RuntimeEnvironment::getSlot(std::string_view name) const {
    auto slot = getSlotIfExists(name);
    uassert(4946305, str::stream() << "environment slot is not registered: " << name, slot);
    return *slot;
}

boost::optional<value::SlotId> RuntimeEnvironment::getSlotIfExists(std::string_view name) const {
    if (auto it = _state->namedSlots.find(name); it != _state->namedSlots.end()) {
        return it->second;
    }

    return boost::none;
}

void RuntimeEnvironment::resetSlot(value::SlotId slot,
                                   value::TypeTags tag,
                                   value::Value val,
                                   bool owned) {
    // With intra-query parallelism enabled the global environment can hold only read-only values.
    tassert(11093406, "Cannot reset slot because parallelism is enabled", !_isSmp);

    if (auto it = _accessors.find(slot); it != _accessors.end()) {
        it->second.reset(value::TagValueMaybeOwned::fromRaw(owned, tag, val));
        return;
    }

    tasserted(4946300, str::stream() << "undefined slot accessor:" << slot);
}

bool RuntimeEnvironment::isSlotRegistered(value::SlotId slot) const {
    return _accessors.count(slot);
}

RuntimeEnvironment::Accessor* RuntimeEnvironment::getAccessor(value::SlotId slot) {
    if (auto it = _accessors.find(slot); it != _accessors.end()) {
        return &it->second;
    }

    tasserted(4946301, str::stream() << "undefined slot accessor:" << slot);
}

const RuntimeEnvironment::Accessor* RuntimeEnvironment::getAccessor(value::SlotId slot) const {
    if (auto it = _accessors.find(slot); it != _accessors.end()) {
        return &it->second;
    }

    tasserted(4946303, str::stream() << "undefined slot accessor:" << slot);
}

std::unique_ptr<RuntimeEnvironment> RuntimeEnvironment::makeCopy() const {
    return std::unique_ptr<RuntimeEnvironment>(new RuntimeEnvironment(*this));
}

std::unique_ptr<RuntimeEnvironment> RuntimeEnvironment::makeDeepCopy() const {
    auto env = std::make_unique<RuntimeEnvironment>();

    env->_state = _state->makeCopyWithoutValues();
    for (auto&& [slotId, index] : _state->slots) {
        // Copy the slot value.
        auto [tag, val] = _accessors.at(slotId).getCopyOfValue().releaseToRaw();

        env->emplaceAccessor(slotId, index);
        env->resetSlot(slotId, tag, val, true /* owned */);
    }
    env->_isSmp = _isSmp;

    return env;
}

std::unique_ptr<RuntimeEnvironment> RuntimeEnvironment::makeCopyForParallelUse() {
    // Once this environment is used to create a copy for a parallel plan execution, it becomes
    // a parallel environment itself.
    _isSmp = true;

    return makeCopy();
}

void RuntimeEnvironment::debugString(StringBuilder* builder,
                                     boost::optional<size_t> lengthCap /*= boost::none*/) const {
    using namespace std::literals;

    value::SlotMap<std::string_view> slotName;
    for (const auto& [name, slot] : _state->namedSlots) {
        slotName[slot] = name;
    }

    std::vector<value::SlotId> slots;
    slots.reserve(_state->slots.size());
    for (const auto& [slot, _] : _state->slots) {
        slots.push_back(slot);
    }
    std::sort(slots.begin(), slots.end());

    StringBuilder tmp;

    tmp << "env: { ";
    if (lengthCap.has_value() && static_cast<size_t>(builder->len() + tmp.len()) > lengthCap) {
        return;
    }
    *builder << tmp.stringData();
    tmp.reset();
    bool first = true;
    for (auto slot : slots) {
        if (first) {
            first = false;
        } else {
            tmp << ", ";
        }

        std::stringstream ss;
        ss << _accessors.at(slot).getViewOfValue();

        tmp << "s" << slot << " = " << ss.str();

        if (auto it = slotName.find(slot); it != slotName.end()) {
            tmp << " (" << it->second << ")";
        }

        if (lengthCap.has_value() && static_cast<size_t>(builder->len() + tmp.len()) > lengthCap) {
            // Truncate this slot's string for explain.
            *builder << "...";
            return;
        }
        *builder << tmp.stringData();
        tmp.reset();
    }
    // Deliberately add the closing curly brace, even if it exceeds lengthCap.
    *builder << " }";
}

std::string RuntimeEnvironment::toDebugString() const {
    StringBuilder builder;
    debugString(&builder);
    return builder.str();
}

}  // namespace mongo::sbe
