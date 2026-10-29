// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/s/metrics/cumulative_metrics_state_holder.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

template <typename... StateEnums>
class CumulativeMetricsStateTracker {
public:
    using AnyState = std::variant<StateEnums...>;
    using StateFieldNameMap = stdx::unordered_map<AnyState, std::string_view>;

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
    static boost::optional<std::string_view> getNameFor(State state,
                                                        const StateFieldNameMap& names) {
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
