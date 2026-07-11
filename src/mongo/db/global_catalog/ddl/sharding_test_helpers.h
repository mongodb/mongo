// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {
namespace sharding_test_helpers {

class Fault {
public:
    Fault(Status error, int triggerCount = 1)
        : _error(std::move(error)), _remainingTriggerCount(triggerCount) {}

    void throwIfEnabled() {
        if (_remainingTriggerCount == 0 || _remainingTriggerCount-- == 0) {
            return;
        }

        uassertStatusOK(_error);
    }

private:
    const Status _error;
    int _remainingTriggerCount{0};
};

class FaultGenerator {
public:
    FaultGenerator() {}

    void appendResponse(Fault result) {
        _mockedResults.push_back(result);
    }

    void getNext() {
        if (!_mockedResults.empty()) {
            auto next = _mockedResults.front();
            _mockedResults.pop_front();
            next.throwIfEnabled();
        }
    }

private:
    std::list<Fault> _mockedResults;
};

}  // namespace sharding_test_helpers
}  // namespace mongo
