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

#include "mongo/util/thread_safe_string.h"

#include <string>

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
        : _showTotal(true), _units(units) {
        _name = name.c_str();
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
        _name = name;
    }
    std::string getName() const {
        return _name.toString();
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
    ThreadSafeString _name;
};

/*
 * Wraps a ProgressMeter and calls finished() when destructed. This may only exist as long as the
 * underlying ProgressMeter, which is owned by CurOp.
 */
class ProgressMeterHolder {
    ProgressMeterHolder(const ProgressMeterHolder&) = delete;
    ProgressMeterHolder& operator=(const ProgressMeterHolder&) = delete;

public:
    ProgressMeterHolder() : _pm(nullptr) {}

    ProgressMeterHolder(ProgressMeter& pm) : _pm(&pm) {}

    ~ProgressMeterHolder() {
        if (_pm) {
            _pm->finished();
        }
    }

    void set(ProgressMeter& pm) {
        _pm = &pm;
    }

    ProgressMeter* operator->() {
        return _pm;
    }

    ProgressMeter* get() {
        return _pm;
    }

    bool hit(int n = 1) {
        return _pm->hit(n);
    }

    void finished() {
        _pm->finished();
    }

private:
    ProgressMeter* _pm;
};
}  // namespace mongo
