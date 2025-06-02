/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/s/metrics/cumulative_metrics_state_holder.h"
#include "mongo/idl/idl_parser.h"

namespace mongo {

template <typename... StateEnums>
class CumulativeMetricsStateTracker {
public:
    using AnyState = std::variant<StateEnums...>;
    using StateFieldNameMap = stdx::unordered_map<AnyState, StringData>;

    template <typename T>
    void onStateTransition(boost::optional<T> before, boost::optional<T> after) {
        getHolderFor<T>().onStateTransition(before, after);
    }

    template <typename T>
    int64_t getCountInState(T state) const {
        return getHolderFor<T>().getStateCounter(state)->load();
    }

    void reportCountsForAllStates(const StateFieldNameMap& names, BSONObjBuilder* bob) const {
        (reportCountsForStatesIn<StateEnums>(names, bob), ...);
    }

    template <typename State>
    static boost::optional<StringData> getNameFor(State state, const StateFieldNameMap& names) {
        auto it = names.find(state);
        if (it == names.end()) {
            return boost::none;
        }
        return it->second;
    }

private:
    template <typename StateEnum>
    using HolderType = CumulativeMetricsStateHolder<StateEnum, idlEnumCount<StateEnum>>;

    template <typename StateEnum>
    auto& getHolderFor() const {
        return get<HolderType<StateEnum>>(_holders);
    }

    template <typename StateEnum>
    auto& getHolderFor() {
        return get<HolderType<StateEnum>>(_holders);
    }

    template <typename StateEnum>
    void reportCountsForStatesIn(const StateFieldNameMap& names, BSONObjBuilder* bob) const {
        for (size_t i = 0; i < idlEnumCount<StateEnum>; ++i) {
            auto state = static_cast<StateEnum>(i);
            if (auto name = getNameFor(state, names); name.has_value()) {
                bob->append(*name, getCountInState(state));
            }
        }
    }

    std::tuple<HolderType<StateEnums>...> _holders;
};

}  // namespace mongo
