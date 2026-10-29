// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo {

template <typename StateVariant, typename CumulativeMetricsT>
class MetricsStateHolder {
public:
    template <typename T>
    MetricsStateHolder(CumulativeMetricsT* cumulativeMetrics, T state)
        : _cumulativeMetrics(cumulativeMetrics) {
        setState(state);
    }

    StateVariant getState() const {
        return _state.load();
    }

    template <typename T>
    void onStateTransition(T before, boost::none_t after) {
        _cumulativeMetrics->template onStateTransition<T>(before, boost::none);
    }

    template <typename T>
    void onStateTransition(boost::none_t before, T after) {
        setState(after);
        _cumulativeMetrics->template onStateTransition<T>(boost::none, after);
    }

    template <typename T>
    void onStateTransition(T before, T after) {
        if (_state.load() != before) {
            // We should only update the state if it matches the expected 'before' state.
            return;
        }
        setState(after);
        _cumulativeMetrics->template onStateTransition<T>(before, after);
    }

private:
    using AtomicState = Atomic<StateVariant>;
    template <typename T>
    void setState(T nextState) {
        static_assert(std::is_assignable_v<decltype(_state.load()), T>);
        _state.store(nextState);
    }
    AtomicState _state;
    CumulativeMetricsT* _cumulativeMetrics;
};
}  // namespace mongo
