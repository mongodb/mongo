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

#include "mongo/base/string_data.h"
#include "mongo/util/modules.h"

namespace mongo::otel::traces {
class SpanNameMaker;

/** Helper to implement the passkey idiom. */
template <typename T>
class MONGO_MOD_PUBLIC Passkey {
private:
    friend T;
    constexpr Passkey() = default;
};

/** Wrapper class around a string to ensure `SpanName`s are only constructed in certain places. */
class MONGO_MOD_PUBLIC SpanName {
public:
    /**
     * Note that this can only be constructed by code allowed to access the passkey. N&O must have
     * ownership of the files defining and instantiating the Passkey types. Additional Passkey types
     * are meant to facilitate cases where the span names should not be visible outside some
     * module, in order to prevent leaking information related to that module.
     */
    constexpr SpanName(StringData name, Passkey<SpanNameMaker>) : _name(name) {}

    constexpr StringData getName() const {
        return _name;
    }

    constexpr bool operator==(const SpanName& other) const {
        return getName() == other.getName();
    }

private:
    StringData _name;
};

/** Helper to create SpanName instances. */
class MONGO_MOD_FILE_PRIVATE SpanNameMaker{public : static constexpr SpanName make(StringData name){
    return SpanName(name, Passkey<SpanNameMaker>{});
}  // namespace mongo::otel::traces
}
;

/**
 * Central registry of OpenTelemetry span names used in the server. When adding a new span to the
 * server, please add an entry to SpanNames grouped under your team name.
 *
 * This ensures that the N&O team has full ownership over new OTel spans in the server for
 * centralized collaboration with downstream OTel consumers.
 */
class MONGO_MOD_PUBLIC SpanNames {
public:
    // Test-only
    static constexpr SpanName kTest1 = SpanNameMaker::make("test_only.span1");
    static constexpr SpanName kTest2 = SpanNameMaker::make("test_only.span2");
    static constexpr SpanName kTest3 = SpanNameMaker::make("test_only.span3");
};

}  // namespace mongo::otel::traces
