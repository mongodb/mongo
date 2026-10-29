// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/write_concern_waiter_list.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"
#include "mongo/util/observable_mutex_registry.h"
#include "mongo/util/overloaded_visitor.h"

#include <utility>
#include <vector>

namespace mongo {
namespace repl {
namespace {

// {waiter, status} pairs collected under the waiter-list mutex and fulfilled afterwards off the
// mutex, so promise emplacement (and any inline continuation) never runs under the lock.
using FulfillBatch = std::vector<std::pair<SharedWaiterHandle, Status>>;

void fulfillWaiter(const SharedWaiterHandle& waiter, const Status& status) {
    if (status.isOK()) {
        waiter->promise.emplaceValue();
    } else {
        waiter->promise.setError(status);
    }
}

// Applies a precomputed per-WriteConcern fulfillment decision to `waiterList`, moving the waiters
// to wake into `toFulfill` (they are erased from the list). Returns the number of waiters woken.
// The `WithLock` makes it explicit that the WriteConcernWaiterList mutex must be held while calling
// this (it mutates `waiterList`). The waiters are fulfilled by the caller afterwards, off the lock.
int applyFulfillment(WithLock,
                     const WriteConcernFulfillmentMap& fulfillmentMap,
                     WaiterListMap& waiterList,
                     FulfillBatch& toFulfill) {
    int woken = 0;
    for (auto& [wc, sublist] : waiterList) {
        auto mapIt = fulfillmentMap.find(wc);
        if (mapIt == fulfillmentMap.end()) {
            // No entry means none of this write concern's waiters can be woken.
            continue;
        }

        std::visit(
            OverloadedVisitor{[&](const OpTime& maxOpTime) {
                                  if (maxOpTime.isNull()) {
                                      return;
                                  }
                                  // Wake all waiters with an opTime <= `maxOpTime` (an
                                  // OpTime-sorted prefix).
                                  auto it = sublist.begin();
                                  for (; it != sublist.end() && it->first <= maxOpTime; ++it) {
                                      toFulfill.emplace_back(std::move(it->second), Status::OK());
                                      ++woken;
                                  }
                                  sublist.erase(sublist.begin(), it);
                              },
                              [&](bool satisfied) {
                                  // A boolean entry is only ever populated as true (wake all of
                                  // this write concern's waiters); an unsatisfied condition is a
                                  // null OpTime instead.
                                  invariant(satisfied);
                                  for (auto& [opTime, waiter] : sublist) {
                                      toFulfill.emplace_back(std::move(waiter), Status::OK());
                                      ++woken;
                                  }
                                  sublist.clear();
                              },
                              [&](const Status& error) {
                                  for (auto& [opTime, waiter] : sublist) {
                                      toFulfill.emplace_back(std::move(waiter), error);
                                      ++woken;
                                  }
                                  sublist.clear();
                              }},
            mapIt->second);
    }
    return woken;
}

}  // namespace

WriteConcernWaiterList::WriteConcernWaiterList(Counter64& waiterCount) : _waiterCount(waiterCount) {
    ObservableMutexRegistry::get().add("replWriteConcernWaiterListMutex", _mutex);
}

std::pair<SharedSemiFuture<void>, SharedWaiterHandle> WriteConcernWaiterList::add(
    const OpTime& opTime, WriteConcernOptions wc) {
    auto pf = makePromiseFuture<void>();
    auto waiter = std::make_shared<Waiter>(std::move(pf.promise), std::move(wc));
    add(opTime, waiter);
    return std::make_pair(std::move(pf.future), std::move(waiter));
}

void WriteConcernWaiterList::add(const OpTime& opTime, SharedWaiterHandle waiter) {
    invariant(waiter && waiter->writeConcern);
    clang_checked::lock_guard lk(_mutex);
    auto [listIt, isNewWriteConcern] = _waiterList.try_emplace(*waiter->writeConcern);
    listIt->second.emplace(opTime, std::move(waiter));
    if (isNewWriteConcern) {
        // Copy-on-write: a caller still holding the previous vector keeps a valid view of it.
        auto writeConcerns = std::make_shared<std::vector<WriteConcernOptions>>(*_writeConcerns);
        writeConcerns->push_back(listIt->first);
        _writeConcerns = std::move(writeConcerns);
    }
    _waiterCount.incrementRelaxed(1);
}

bool WriteConcernWaiterList::remove(const OpTime& opTime, SharedWaiterHandle waiter) {
    if (!waiter->writeConcern) {
        return false;
    }
    clang_checked::lock_guard lk(_mutex);
    auto listIt = _waiterList.find(*waiter->writeConcern);
    if (listIt == _waiterList.end()) {
        return false;
    }
    auto& sublist = listIt->second;
    auto [begin, end] = sublist.equal_range(opTime);
    for (auto it = begin; it != end; ++it) {
        if (it->second == waiter) {
            sublist.erase(it);  // An emptied bucket is left in place.
            _waiterCount.decrementRelaxed(1);
            return true;
        }
    }
    return false;
}

void WriteConcernWaiterList::setErrorAll(Status status) {
    invariant(!status.isOK());
    WaiterListMap taken;
    {
        clang_checked::lock_guard lk(_mutex);
        taken.swap(_waiterList);
        _writeConcerns = std::make_shared<const std::vector<WriteConcernOptions>>();
        _waiterCount.setToZero();
    }
    // Fulfill off the lock.
    for (auto& [wc, sublist] : taken) {
        for (auto& [opTime, waiter] : sublist) {
            waiter->promise.setError(status);
        }
    }
}

void WriteConcernWaiterList::setValueIf(const WriteConcernFulfillmentMap& fulfillmentMap) {
    FulfillBatch toFulfill;
    {
        clang_checked::lock_guard lk(_mutex);
        const int woken = applyFulfillment(lk, fulfillmentMap, _waiterList, toFulfill);
        _waiterCount.decrementRelaxed(woken);
    }
    // Fulfill waiter promises off the lock, so continuations never run under it.
    for (auto& [waiter, status] : toFulfill) {
        fulfillWaiter(waiter, status);
    }
}

std::shared_ptr<const std::vector<WriteConcernOptions>> WriteConcernWaiterList::getWriteConcerns()
    const {
    clang_checked::lock_guard lk(_mutex);
    return _writeConcerns;
}

size_t WriteConcernWaiterList::numWaiters(const WriteConcernOptions& writeConcern) const {
    clang_checked::lock_guard lk(_mutex);
    // find() rather than operator[], which would insert an empty bucket for a WriteConcern the list
    // has never seen.
    auto listIt = _waiterList.find(writeConcern);
    return listIt == _waiterList.end() ? 0 : listIt->second.size();
}

}  // namespace repl
}  // namespace mongo
