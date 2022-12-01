/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <string>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/db/operation_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

class ProgressMeter {
    ProgressMeter(const ProgressMeter&) = delete;
    ProgressMeter& operator=(const ProgressMeter&) = delete;

public:
    ProgressMeter(unsigned long long total,
                  int secondsBetween = 3,
                  int checkInterval = 100,
                  std::string units = "",
                  std::string name = "Progress")
        : _showTotal(true), _units(units), _name(std::move(name)) {
        reset(total, secondsBetween, checkInterval);
    }

    ProgressMeter() {
        _name = "Progress";
    }

    // typically you do ProgressMeterHolder
    void reset(unsigned long long total, int secondsBetween = 3, int checkInterval = 100);

    void finished() {
        _active = false;
    }
    bool isActive() const {
        return _active;
    }

    /**
     * @param n how far along we are relative to the total # we set in CurOp::setMessage
     * @return if row was printed
     */
    bool hit(int n = 1);

    void setUnits(const std::string& units) {
        _units = units;
    }
    std::string getUnit() const {
        return _units;
    }

    void setName(StringData name) {
        stdx::lock_guard lk(_nameMutex);
        _name = std::string{name};
    }
    std::string getName() const {
        stdx::lock_guard lk(_nameMutex);
        return _name;
    }

    void setTotalWhileRunning(unsigned long long total) {
        _total = total;
    }

    unsigned long long done() const {
        return _done;
    }

    unsigned long long hits() const {
        return _hits;
    }

    unsigned long long total() const {
        return _total;
    }

    void showTotal(bool doShow) {
        _showTotal = doShow;
    }

    std::string toString() const;

    bool operator==(const ProgressMeter& other) const {
        return this == &other;
    }

private:
    bool _active{false};

    unsigned long long _total;
    bool _showTotal{true};
    int _secondsBetween{3};
    int _checkInterval{100};

    unsigned long long _done;
    unsigned long long _hits;
    int _lastTime;

    std::string _units;

    mutable stdx::mutex _nameMutex;  // NOLINT
    std::string _name;               // guarded by _nameMutex
};

/*
 * Wraps a CurOp owned ProgressMeter and calls finished() when destructed. This may only exist as
 * long as the underlying ProgressMeter.
 *
 * The underlying ProgressMeter will have the same locking requirements as CurOp (see CurOp class
 * description). Accessors and modifiers on the underlying ProgressMeter may need to be performed
 * while holding a client lock and specifying the WithLock argument. If accessing without a client
 * lock, then the thread must be executing the associated OperationContext.
 */
class ProgressMeterHolder {
    ProgressMeterHolder(const ProgressMeterHolder&) = delete;
    ProgressMeterHolder& operator=(const ProgressMeterHolder&) = delete;

public:
    ProgressMeterHolder() : _pm(nullptr) {}

    ~ProgressMeterHolder() {
        if (_pm) {
            {
                stdx::unique_lock<Client> lk(*_opCtx->getClient());
                _pm->finished();
            }
        }
    }

    void set(WithLock, ProgressMeter& pm, OperationContext* opCtx) {
        _opCtx = opCtx;
        _pm = &pm;
    }

    ProgressMeter* get(WithLock) {
        return _pm;
    }

private:
    ProgressMeter* _pm;
    OperationContext* _opCtx;
};
}  // namespace mongo
