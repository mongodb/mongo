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
#include "mongo/otel/metrics/metrics_metric.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo::otel::metrics {

template <typename T>
class MONGO_MOD_PUBLIC Counter : public Metric {
public:
    ~Counter() override = default;

    // T must be nonnegative.
    virtual void add(T value) = 0;

    virtual T value() const = 0;

    virtual const std::string& name() const = 0;

    virtual const std::string& description() const = 0;

    virtual const std::string& unit() const = 0;
};

// A lock free (non-decreasing) counter and metadata about it.
template <typename T>
class CounterImpl : public Counter<T> {
public:
    CounterImpl(std::string name, std::string description, std::string unit);

    void add(T value) override;

    T value() const override {
        return _value.load();
    }

    const std::string& name() const override {
        return _name;
    }

    const std::string& description() const override {
        return _description;
    }

    const std::string& unit() const override {
        return _unit;
    }

    BSONObj serializeToBson(const std::string& key) const override;

private:
    std::string _name;
    std::string _description;
    std::string _unit;
    Atomic<T> _value;
};

///////////////////////////////////////////////////////////////////////////////
// Implementation details
///////////////////////////////////////////////////////////////////////////////

template <typename T>
CounterImpl<T>::CounterImpl(std::string name, std::string description, std::string unit)
    : _name(std::move(name)), _description(std::move(description)), _unit(std::move(unit)) {}

template <typename T>
void CounterImpl<T>::add(T value) {
    massert(ErrorCodes::BadValue, "Counter increment must be nonnegative", value >= 0);
    _value.fetchAndAddRelaxed(value);
}

template <typename T>
BSONObj CounterImpl<T>::serializeToBson(const std::string& key) const {
    return BSON(key << _value.loadRelaxed());
}
}  // namespace mongo::otel::metrics
