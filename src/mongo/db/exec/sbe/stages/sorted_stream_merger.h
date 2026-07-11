// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/util/modules.h"

#include <queue>

namespace mongo::sbe {
/**
 * Merges the outputs of N branches, each of which is sorted. The output is also sorted.
 *
 * Each branch has a pointer to a "stream" which must implement a getNext() method like so:
 *
 * class MyStream {
 * public:
 *     PlanState getNext() { ... }
 * };
 *
 * A stream may be a PlanStage but it does not have to be.
 */
template <class SortedStream>
class SortedStreamMerger final {
public:
    SortedStreamMerger(std::vector<std::vector<value::SlotAccessor*>> inputKeyAccessors,
                       std::vector<SortedStream*> streams,
                       std::vector<value::SortDirection> dirs,
                       std::vector<value::SwitchAccessor>& outAccessors)
        : _dirs(convertDirs(dirs)), _outAccessors(outAccessors), _heap(&_dirs) {
        tassert(11094706,
                "Expect number of input key lists to match the number of streams",
                inputKeyAccessors.size() == streams.size());
        tassert(11094705, "Expect non-empty streams list", !streams.empty());
        const auto keySize = inputKeyAccessors.front().size();

        for (size_t i = 0; i < streams.size(); ++i) {
            tassert(11094704,
                    "Expect all input key lists to be of the same length",
                    keySize == inputKeyAccessors[i].size());

            _branches.push_back(Branch{streams[i], std::move(inputKeyAccessors[i]), i});
        }
    }

    void clear() {
        _heap = decltype(_heap)(&_dirs);
    }

    void init() {
        clear();
        for (auto&& branch : _branches) {
            if (branch.stream->getNext() == PlanState::ADVANCED) {
                _heap.push(&branch);
            }
        }
        _lastBranchPopped = nullptr;
    }

    PlanState getNext() {
        if (_lastBranchPopped && _lastBranchPopped->stream->getNext() == PlanState::ADVANCED) {
            // This branch was removed in the last call to getNext() on the stage.
            _heap.push(_lastBranchPopped);
            _lastBranchPopped = nullptr;
        } else if (_heap.empty()) {
            return PlanState::IS_EOF;
        }

        auto top = _heap.top();
        _heap.pop();
        _lastBranchPopped = top;

        for (size_t i = 0; i < _outAccessors.size(); ++i) {
            _outAccessors[i].setIndex(top->branchSwitchId);
        }

        return PlanState::ADVANCED;
    }

private:
    struct Branch {
        SortedStream* stream = nullptr;

        std::vector<value::SlotAccessor*> inputKeyAccessors;
        size_t branchSwitchId{0};
    };

    class BranchComparator {
    public:
        BranchComparator(const std::vector<int>* dirs) : _dirs(dirs) {}

        bool operator()(const Branch*, const Branch*);

    private:
        // Guaranteed not to be not null.
        const std::vector<int>* _dirs;
    };

    static std::vector<int> convertDirs(const std::vector<value::SortDirection>& dirs) {
        std::vector<int> outDirs;
        for (auto&& dir : dirs) {
            outDirs.push_back(dir == value::SortDirection::Ascending ? 1 : -1);
        }
        return outDirs;
    }

    const std::vector<int> _dirs;

    // Switched output.
    std::vector<value::SwitchAccessor>& _outAccessors;

    // Branches are owned here.
    std::vector<Branch> _branches;

    // Heap for maintaining order.
    std::priority_queue<Branch*, std::vector<Branch*>, BranchComparator> _heap;

    // Indicates the last branch which we popped from. At the beginning of a getNext() call, this
    // branch will _not_ be in the heap and must be reinserted. This is done to avoid calling
    // getNext() on the branch whose value is being returned, which would require an extra copy of
    // the output value.
    Branch* _lastBranchPopped = nullptr;
};

template <class SortedStream>
bool SortedStreamMerger<SortedStream>::BranchComparator::operator()(const Branch* left,
                                                                    const Branch* right) {
    // Because this comparator is used with std::priority_queue, which is a max heap,
    // return _true_ when left > right.
    for (size_t i = 0; i < left->inputKeyAccessors.size(); ++i) {
        auto lhsTagVal = left->inputKeyAccessors[i]->getViewOfValue();
        auto rhsTagVal = right->inputKeyAccessors[i]->getViewOfValue();

        auto [tag, val] =
            value::compareValue(lhsTagVal.tag, lhsTagVal.value, rhsTagVal.tag, rhsTagVal.value);

        uassert(5073804,
                str::stream() << "Could not compare values with type " << lhsTagVal.tag << " and "
                              << rhsTagVal.tag,
                tag == value::TypeTags::NumberInt32);
        const auto result = value::bitcastTo<int32_t>(val) * ((*_dirs)[i]);
        if (result < 0) {
            return false;
        }
        if (result > 0) {
            return true;
        }
    }

    return false;
}
}  // namespace mongo::sbe
