/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/otel/metrics/metrics_attributes.h"
#include "mongo/util/modules.h"

namespace mongo::otel::metrics {

/** UpDownCounter interface with typed attributes. add() accepts any delta, including negative. */
template <typename T, AttributeType... AttributeTs>
class MONGO_MOD_PUBLIC UpDownCounter {
public:
    using Attributes = std::tuple<AttributeTs...>;
    virtual ~UpDownCounter() = default;

    virtual void add(T value, const Attributes& attributes) = 0;
};

/** Specialization when there are no attributes, adding a convenience add(T) overload. */
template <typename T>
class MONGO_MOD_PUBLIC UpDownCounter<T> {
public:
    using Attributes = std::tuple<>;
    virtual ~UpDownCounter() = default;

    void add(T value) {
        add(value, {});
    }

protected:
    virtual void add(T value, const std::tuple<>& attributes) = 0;
};

}  // namespace mongo::otel::metrics
