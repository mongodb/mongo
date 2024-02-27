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

#pragma once

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/exec/sbe/abt/slots_provider.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

namespace mongo::sbe {
/**
 * A holder for "global" slots and accessors. These are used in a PlanStage tree but:
 *  - Cannot be made constants due to restrictions on the lifetime of such values (e.g., they're
 *    singleton instances owned somewhere else).
 *  - Can be changed in runtime outside of the PlanStage tree (e.g., a resume recordId changed by a
 *    PlanExecutor).
 *
 * A RuntimeEnvironment object is created once per an execution thread. That means that each
 * producer and consumer in a parallel plan will have their own compilation environment, with their
 * own slot accessors. However, slot accessors in each such environment will access shared data,
 * which is the same across all environments.
 *
 * To avoid data races, the values stored in the runtime environment are considered read-only when
 * used with a parallel plan. An attempt to change any slot with 'resetValue' will result in a user
 * exception.
 *
 * If the runtime environment is used in a serial plan, modification of the slots is allowed.
 */
using InputParamToSlotMap = stdx::unordered_map<MatchExpression::InputParamId, sbe::value::SlotId>;
class RuntimeEnvironment final : public optimizer::SlotsProvider {
public:
    RuntimeEnvironment() = default;
    RuntimeEnvironment(RuntimeEnvironment&&) = delete;
    RuntimeEnvironment& operator=(const RuntimeEnvironment&) = delete;
    RuntimeEnvironment& operator=(const RuntimeEnvironment&&) = delete;
    virtual ~RuntimeEnvironment();

    class Accessor final : public value::SlotAccessor {
    public:
        Accessor(RuntimeEnvironment* env, size_t index) : _env{env}, _index{index} {}

        std::pair<value::TypeTags, value::Value> getViewOfValue() const override {
            auto [owned, tag, val] = _env->_state->values[_index];
            return {tag, val};
        }

        std::pair<value::TypeTags, value::Value> copyOrMoveValue() override {
            // Always make a copy.
            auto [owned, tag, val] = _env->_state->values[_index];
            return copyValue(tag, val);
        }

        void reset(bool owned, value::TypeTags tag, value::Value val) {
            release();

            _env->_state->values[_index] = {owned, tag, val};
        }

    private:
        void release() {
            auto [owned, tag, val] = _env->_state->values[_index];
            if (owned) {
                releaseValue(tag, val);
                _env->_state->values[_index] = {false, value::TypeTags::Nothing, 0};
            }
        }

        RuntimeEnvironment* const _env;
        const size_t _index;
    };

    /**
     * Registers and returns a SlotId for the given slot 'name'. The 'slotIdGenerator' is used
     * to generate a new SlotId for the given slot 'name', which is then registered with this
     * environment by creating a new SlotAccessor. The value 'val' is then stored within the
     * SlotAccessor and the newly generated SlotId is returned.
     *
     * Both owned and unowned values can be stored in the runtime environment.
     *
     * A user exception is raised if this slot 'name' has been already registered.
     */
    value::SlotId registerSlot(StringData name,
                               value::TypeTags tag,
                               value::Value val,
                               bool owned,
                               value::SlotIdGenerator* slotIdGenerator);

    /**
     * Same as above, but allows to register an unnamed slot.
     */
    value::SlotId registerSlot(value::TypeTags tag,
                               value::Value val,
                               bool owned,
                               value::SlotIdGenerator* slotIdGenerator) final;

    /**
     * Returns a SlotId registered for the given slot 'name'. If the slot with the specified name
     * hasn't been registered, a user exception is raised.
     */
    value::SlotId getSlot(StringData name) const final;

    /**
     * Returns a SlotId registered for the given slot 'name'. If the slot with the specified name
     * hasn't been registered, boost::none is returned.
     */
    boost::optional<value::SlotId> getSlotIfExists(StringData name) const final;

    /**
     * Store the given value in the specified slot within this runtime environment instance.
     *
     * A user exception is raised if the SlotId is not registered within this environment, or
     * if this environment is used with a parallel plan.
     */
    void resetSlot(value::SlotId slot, value::TypeTags tag, value::Value val, bool owned);

    bool isSlotRegistered(value::SlotId slot) const;

    /**
     * Returns a SlotAccessor for the given SlotId which must be previously registered within this
     * Environment by invoking 'registerSlot' method.
     *
     * A user exception is raised if the SlotId is not registered within this environment.
     */
    Accessor* getAccessor(value::SlotId slot);
    const Accessor* getAccessor(value::SlotId slot) const;

    /**
     * Make a copy of this environment. The new environment will have its own set of SlotAccessors
     * pointing to the same shared data holding slot values.
     *
     * To create a copy of the runtime environment for a parallel execution plan, please use
     * makeCopyForParallelUse() method. This will result in this environment being converted to a
     * parallel environment, as well as the newly created copy.
     */
    std::unique_ptr<RuntimeEnvironment> makeCopyForParallelUse();
    std::unique_ptr<RuntimeEnvironment> makeCopy() const;

    /**
     * Make a "deep" copy of this environment. The new environment will have its own set of
     * SlotAccessors pointing to data copied from this RuntimeEnvironment. All the slot values are
     * made owned by the new environment as much as possible. There could be some uncopyable types
     * which can not be owned by the new environment, e.g. TimeZoneDatabase.
     */
    std::unique_ptr<RuntimeEnvironment> makeDeepCopy() const;

    /**
     * Dumps all the slots currently defined in this environment into the given string builder.
     */
    void debugString(StringBuilder* builder) const;
    std::string toDebugString() const;

private:
    RuntimeEnvironment(const RuntimeEnvironment&);

    struct State {
        size_t pushSlot(value::SlotId slot) {
            auto index = values.size();

            values.push_back({false, value::TypeTags::Nothing, 0});

            auto [_, inserted] = slots.emplace(slot, index);
            uassert(4946302, str::stream() << "duplicate environment slot: " << slot, inserted);
            return index;
        }

        void nameSlot(StringData name, value::SlotId slot) {
            uassert(5645901, str::stream() << "undefined slot: " << slot, slots.count(slot));
            auto [_, inserted] = namedSlots.emplace(name, slot);
            uassert(5645902, str::stream() << "duplicate named slot: " << name, inserted);
        }

        std::unique_ptr<State> makeCopyWithoutValues() {
            auto state = std::make_unique<State>();
            state->namedSlots = namedSlots;
            state->slots = slots;

            state->values.resize(values.size());

            // Populate slot values with default value.
            std::fill(
                state->values.begin(),
                state->values.end(),
                FastTuple<bool, value::TypeTags, value::Value>{false, value::TypeTags::Nothing, 0});

            return state;
        }

        StringMap<value::SlotId> namedSlots;
        value::SlotMap<size_t> slots;

        std::vector<FastTuple<bool, value::TypeTags, value::Value>> values;
    };

    void emplaceAccessor(value::SlotId slot, size_t index) {
        _accessors.emplace(slot, Accessor{this, index});
    }

    std::shared_ptr<State> _state{std::make_shared<State>()};
    value::SlotMap<Accessor> _accessors;
    bool _isSmp{false};

    friend class Accessor;
};
}  // namespace mongo::sbe
