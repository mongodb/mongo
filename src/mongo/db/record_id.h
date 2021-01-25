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

#include <boost/functional/hash.hpp>
#include <boost/optional.hpp>
#include <climits>
#include <cstdint>
#include <fmt/format.h>
#include <ostream>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/bufreader.h"

namespace mongo {


/**
 * The key that uniquely identifies a Record in a Collection or RecordStore.
 */
class RecordId {
public:
    // This set of constants define the boundaries of the 'normal' and 'reserved' id ranges for
    // the kLong format.
    static constexpr int64_t kMinRepr = LLONG_MIN;
    static constexpr int64_t kMaxRepr = LLONG_MAX;
    static constexpr int64_t kMinReservedRepr = kMaxRepr - (1024 * 1024);

    // OID Constants
    static constexpr unsigned char kMinOID[OID::kOIDSize] = {0x00};
    static constexpr unsigned char kMaxOID[OID::kOIDSize] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    // This reserved range leaves 2^20 possible reserved values.
    static constexpr unsigned char kMinReservedOID[OID::kOIDSize] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf0, 0x00, 0x00};

    /**
     * A RecordId that compares less than all ids for a given data format.
     */
    template <typename T>
    static RecordId min() {
        if constexpr (std::is_same_v<T, int64_t>) {
            return RecordId(kMinRepr);
        } else {
            static_assert(std::is_same_v<T, OID>, "Unsupported RecordID format");
            return RecordId(OID(kMinOID));
        }
    }

    /**
     * A RecordId that compares greater than all ids that represent documents in a collection.
     */
    template <typename T>
    static RecordId max() {
        if constexpr (std::is_same_v<T, int64_t>) {
            return RecordId(kMaxRepr);
        } else {
            static_assert(std::is_same_v<T, OID>, "Unsupported RecordID format");
            return RecordId(OID(kMaxOID));
        }
    }

    /**
     * Returns the first record in the reserved id range at the top of the RecordId space.
     */
    template <typename T>
    static RecordId minReserved() {
        if constexpr (std::is_same_v<T, int64_t>) {
            return RecordId(kMinReservedRepr);
        } else {
            static_assert(std::is_same_v<T, OID>, "Unsupported RecordID format");
            return RecordId(OID(kMinReservedOID));
        }
    }

    /**
     * Enumerates all reserved ids that have been allocated for a specific purpose.
     * The underlying value of the reserved Record ID is data-format specific and must be retrieved
     * by the getReservedId() helper.
     */
    enum class Reservation { kWildcardMultikeyMetadataId };

    /**
     * Returns the reserved RecordId value for a given Reservation.
     */
    template <typename T>
    static RecordId reservedIdFor(Reservation res) {
        // There is only one reservation at the moment.
        invariant(res == Reservation::kWildcardMultikeyMetadataId);
        if constexpr (std::is_same_v<T, int64_t>) {
            return RecordId(kMinReservedRepr);
        } else {
            static_assert(std::is_same_v<T, OID>, "Unsupported RecordID format");
            return RecordId(OID(kMinReservedOID));
        }
    }

    RecordId() : _format(Format::kNull) {}
    explicit RecordId(int64_t repr) : _storage(repr), _format(Format::kLong) {}
    explicit RecordId(const OID& oid) : _storage(oid), _format(Format::kOid) {}

    /**
     * Construct a RecordId from two halves.
     */
    RecordId(int high, int low) : RecordId((uint64_t(high) << 32) | uint32_t(low)) {}

    /** Tag for dispatching on null values */
    class Null {};

    /**
     * Helpers to dispatch based on the underlying type.
     */
    template <typename OnNull, typename OnLong, typename OnOid>
    auto withFormat(OnNull&& onNull, OnLong&& onLong, OnOid&& onOid) const {
        switch (_format) {
            case Format::kNull:
                return onNull(Null());
            case Format::kLong:
                return onLong(_storage._long);
            case Format::kOid:
                return onOid(_storage._oid);
            default:
                MONGO_UNREACHABLE;
        }
    }

    /**
     * Returns the underlying data for a given format. Will invariant if the RecordId is not storing
     * requested format.
     */
    template <typename T>
    T as() const {
        if constexpr (std::is_same_v<T, int64_t>) {
            // In the the int64_t format, null can also be represented by '0'.
            if (_format == Format::kNull) {
                return 0;
            }
            invariant(_format == Format::kLong);
            return _storage._long;
        } else {
            static_assert(std::is_same_v<T, OID>, "Unsupported RecordID format");
            invariant(_format == Format::kOid);
            return _storage._oid;
        }
    }

    bool isNull() const {
        // In the the int64_t format, null can also represented by '0'.
        if (_format == Format::kLong) {
            return _storage._long == 0;
        }
        return _format == Format::kNull;
    }

    /**
     * Valid RecordIds are the only ones which may be used to represent Records. The range of valid
     * RecordIds includes both "normal" ids that refer to user data, and "reserved" ids that are
     * used internally. All RecordIds outside of the valid range are sentinel values.
     */
    bool isValid() const {
        return isNormal() || isReserved();
    }

    /**
     * Normal RecordIds are those which fall within the range used to represent normal user data,
     * excluding the reserved range at the top of the RecordId space.
     */
    bool isNormal() const {
        return withFormat([](Null n) { return false; },
                          [](int64_t rid) { return rid > 0 && rid < kMinReservedRepr; },
                          [](const OID& oid) { return oid.compare(OID(kMinReservedOID)) < 0; });
    }

    /**
     * Returns true if this RecordId falls within the reserved range at the top of the record space.
     */
    bool isReserved() const {
        return withFormat([](Null n) { return false; },
                          [](int64_t rid) { return rid >= kMinReservedRepr && rid < kMaxRepr; },
                          [](const OID& oid) {
                              return oid.compare(OID(kMinReservedOID)) >= 0 &&
                                  oid.compare(OID(kMaxOID)) < 0;
                          });
    }

    int compare(const RecordId& rhs) const {
        // Null always compares less than every other RecordId.
        if (isNull() && rhs.isNull()) {
            return 0;
        } else if (isNull()) {
            return -1;
        } else if (rhs.isNull()) {
            return 1;
        }
        invariant(_format == rhs._format);
        return withFormat([](Null n) { return 0; },
                          [&](const int64_t rid) {
                              return rid == rhs._storage._long ? 0
                                                               : rid < rhs._storage._long ? -1 : 1;
                          },
                          [&](const OID& oid) { return oid.compare(rhs._storage._oid); });
    }

    size_t hash() const {
        size_t hash = 0;
        withFormat([](Null n) {},
                   [&](int64_t rid) { boost::hash_combine(hash, rid); },
                   [&](const OID& oid) {
                       boost::hash_combine(hash, std::string(oid.view().view(), OID::kOIDSize));
                   });
        return hash;
    }

    std::string toString() const {
        return withFormat([](Null n) { return std::string("null"); },
                          [](int64_t rid) { return std::to_string(rid); },
                          [](const OID& oid) { return oid.toString(); });
    }

    /**
     * Hash value for this RecordId. The hash implementation may be modified, and its behavior
     * may differ across platforms. Hash values should not be persisted.
     */
    struct Hasher {
        size_t operator()(const RecordId& rid) const {
            return rid.hash();
        }
    };

    void serialize(fmt::memory_buffer& buffer) const {
        withFormat([&](Null n) { fmt::format_to(buffer, "RecordId(null)"); },
                   [&](int64_t rid) { fmt::format_to(buffer, "RecordId({})", rid); },
                   [&](const OID& oid) { fmt::format_to(buffer, "RecordId({})", oid.toString()); });
    }

    void serialize(BSONObjBuilder* builder) const {
        withFormat([&](Null n) { builder->append("RecordId", "null"); },
                   [&](int64_t rid) { builder->append("RecordId"_sd, rid); },
                   [&](const OID& oid) { builder->append("RecordId"_sd, oid); });
    }

private:
    /**
     * Specifies the storage format of this RecordId.
     */
    enum class Format : uint32_t {
        /** Contains no value */
        kNull,
        /** int64_t */
        kLong,
        /** OID = char[12] */
        kOid
    };

// Pack our union so that it only uses 12 bytes. The union will default to a 8 byte alignment,
// making it 16 bytes total with 4 bytes of padding. Instead, we force the union to use a 4 byte
// alignment, so it packs into 12 bytes. This leaves 4 bytes for our Format, allowing the RecordId
// to use 16 bytes total.
#pragma pack(push, 4)
    union Storage {
        // Format::kLong
        int64_t _long;
        // Format::kOid
        OID _oid;

        Storage() {}
        Storage(int64_t s) : _long(s) {}
        Storage(const OID& s) : _oid(s) {}
    };
#pragma pack(pop)

    Storage _storage;
    Format _format;
};

inline bool operator==(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs) == 0;
}
inline bool operator!=(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs);
}
inline bool operator<(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs) < 0;
}
inline bool operator<=(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs) <= 0;
}
inline bool operator>(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs) > 0;
}
inline bool operator>=(RecordId lhs, RecordId rhs) {
    return lhs.compare(rhs) >= 0;
}

inline StringBuilder& operator<<(StringBuilder& stream, const RecordId& id) {
    return stream << "RecordId(" << id.toString() << ')';
}

inline std::ostream& operator<<(std::ostream& stream, const RecordId& id) {
    return stream << "RecordId(" << id.toString() << ')';
}

inline std::ostream& operator<<(std::ostream& stream, const boost::optional<RecordId>& id) {
    return stream << "RecordId(" << (id ? id->toString() : 0) << ')';
}

}  // namespace mongo
