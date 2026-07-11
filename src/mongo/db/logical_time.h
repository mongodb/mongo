// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/util/modules.h"

#include <array>
#include <compare>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
using namespace std::literals::string_view_literals;

class BSONObj;
class BSONObjBuilder;

/**
 *  The LogicalTime class holds the cluster time of the cluster. It provides conversions to
 *  a Timestamp to allow integration with opLog.
 */
class LogicalTime {
public:
    static constexpr std::string_view kOperationTimeFieldName = "operationTime"sv;


    /** An uninitialized value of LogicalTime. Default constructed. */
    static const LogicalTime kUninitialized;

    constexpr LogicalTime() = default;
    explicit LogicalTime(Timestamp ts);

    /**
     * Parses the 'operationTime' field of the specified object and extracts a LogicalTime from it.
     * If 'operationTime' is missing or of the wrong type, throws.
     */
    static LogicalTime fromOperationTime(const BSONObj& obj);

    /**
     * Appends "operationTime" field to the specified builder as a Timestamp type.
     */
    void appendAsOperationTime(BSONObjBuilder* builder) const;

    Timestamp asTimestamp() const {
        return Timestamp(_time);
    }

    /**
     * Increases the _time by ticks.
     */
    void addTicks(uint64_t ticks);

    /**
     * Const version, returns the LogicalTime with increased _time by ticks.
     */
    LogicalTime addTicks(uint64_t ticks) const;

    std::string toString() const;

    /**
     * Returns the LogicalTime as an array of unsigned chars in little endian order for use with the
     * crypto::hmacSHA1 function.
     */
    std::array<unsigned char, sizeof(uint64_t)> toUnsignedArray() const;

    /**
     *  serialize into BSON object.
     */
    BSONObj toBSON() const;

    /*
     * These methods support IDL parsing of logical times.
     */
    static LogicalTime parseFromBSON(const BSONElement& elem);
    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* bob) const;

    bool operator==(const LogicalTime& o) const {
        return asTimestamp() == o.asTimestamp();
    }

    auto operator<=>(const LogicalTime& o) const {
        return asTimestamp() <=> o.asTimestamp();
    }

    friend std::ostream& operator<<(std::ostream& s, const LogicalTime& v) {
        return s << v.toString();
    }

private:
    uint64_t _time{0};
};

inline const LogicalTime LogicalTime::kUninitialized{};

}  // namespace mongo
