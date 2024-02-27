/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expressions/runtime_environment.h"

#include <absl/meta/type_traits.h>
#include <boost/none.hpp>
#include <iosfwd>
#include <string_view>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

#include "mongo/bson/util/builder.h"

namespace mongo::sbe {
RuntimeEnvironment::RuntimeEnvironment(const RuntimeEnvironment& other)
    : _state{other._state}, _isSmp{other._isSmp} {
    for (auto&& [slotId, index] : _state->slots) {
        emplaceAccessor(slotId, index);
    }
}

RuntimeEnvironment::~RuntimeEnvironment() {
    if (_state.use_count() == 1) {
        for (size_t idx = 0; idx < _state->values.size(); ++idx) {
            auto [owned, tag, val] = _state->values[idx];
            if (owned) {
                releaseValue(tag, val);
            }
        }
    }
}

value::SlotId RuntimeEnvironment::registerSlot(StringData name,
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
    emplaceAccessor(slot, _state->pushSlot(slot));
    _accessors.at(slot).reset(owned, tag, val);
    return slot;
}

value::SlotId RuntimeEnvironment::getSlot(StringData name) const {
    auto slot = getSlotIfExists(name);
    uassert(4946305, str::stream() << "environment slot is not registered: " << name, slot);
    return *slot;
}

boost::optional<value::SlotId> RuntimeEnvironment::getSlotIfExists(StringData name) const {
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
    invariant(!_isSmp);

    if (auto it = _accessors.find(slot); it != _accessors.end()) {
        it->second.reset(owned, tag, val);
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
        auto [tag, val] = _accessors.at(slotId).getCopyOfValue();

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

void RuntimeEnvironment::debugString(StringBuilder* builder) const {
    using namespace std::literals;

    value::SlotMap<StringData> slotName;
    for (const auto& [name, slot] : _state->namedSlots) {
        slotName[slot] = name;
    }

    std::vector<value::SlotId> slots;
    slots.reserve(_state->slots.size());
    for (const auto& [slot, _] : _state->slots) {
        slots.push_back(slot);
    }
    std::sort(slots.begin(), slots.end());

    *builder << "env: { ";
    bool first = true;
    for (auto slot : slots) {
        if (first) {
            first = false;
        } else {
            *builder << ", ";
        }

        std::stringstream ss;
        ss << _accessors.at(slot).getViewOfValue();

        *builder << "s" << slot << " = " << ss.str();

        if (auto it = slotName.find(slot); it != slotName.end()) {
            *builder << " (" << it->second << ")";
        }
    }
    *builder << " }";
}

std::string RuntimeEnvironment::toDebugString() const {
    StringBuilder builder;
    debugString(&builder);
    return builder.str();
}

}  // namespace mongo::sbe
