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

#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

namespace mongo::otel::metrics {

template <typename T>
class MONGO_MOD_PUBLIC Gauge {
public:
    virtual ~Gauge() = default;

    // T must be nonnegative.
    virtual void set(T value) = 0;

    virtual T value() const = 0;
};

// A lock free Gauge and metadata about it.
template <typename T>
class GaugeImpl : public Gauge<T> {
public:
    void set(T value) override;

    T value() const override {
        return _value.load();
    }

private:
    Atomic<T> _value;
};

///////////////////////////////////////////////////////////////////////////////
// Implementation details
///////////////////////////////////////////////////////////////////////////////

template <typename T>
void GaugeImpl<T>::set(T value) {
    _value.storeRelaxed(value);
}

}  // namespace mongo::otel::metrics
