// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/data_view.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <cstring>
#include <iosfwd>
#include <string>
#include <string_view>

#include <sys/types.h>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
class SecureRandom;

/**
 * Object ID type.
 * BSON objects typically have an _id field for the object id. This field should be the first
 * member of the object when present. The OID class is a special type that is a 12 byte id which
 * is likely to be unique to the system.  You may also use other types for _id's.
 * When _id field is missing from a BSON object, on an insert the database may insert one
 * automatically in certain circumstances.
 *
 * The BSON ObjectID is a 12-byte value consisting of a 4-byte timestamp (seconds since epoch),
 * in the highest order 4 bytes followed by a 5 byte value unique to this machine AND process,
 * followed by a 3 byte counter.
 *
 *               4 byte timestamp    5 byte process unique   3 byte counter
 *             |<----------------->|<---------------------->|<------------->
 * OID layout: [----|----|----|----|----|----|----|----|----|----|----|----]
 *             0                   4                   8                   12
 *
 * The timestamp is a big endian 4 byte signed-integer.
 *
 * The process unique is an arbitrary sequence of 5 bytes. There are no endianness concerns
 * since it is never interpreted as a multi-byte value.
 *
 * The counter is a big endian 3 byte unsigned integer.
 *
 * Note: The timestamp and counter are big endian (in contrast to the rest of BSON) because
 * we use memcmp to order OIDs, and we want to ensure an increasing order.
 *
 * Warning: You MUST call OID::justForked() after a fork(). This ensures that each process will
 * generate unique OIDs.
 */
class OID {
public:
    OID() : _data() {}

    enum { kOIDSize = 12, kTimestampSize = 4, kInstanceUniqueSize = 5, kIncrementSize = 3 };

    /** init from a 24 char hex string */
    explicit OID(std::string_view s) {
        init(s);
    }

    /** init from a reference to a 12-byte array */
    explicit OID(const unsigned char (&arr)[kOIDSize]) {
        std::memcpy(_data, arr, sizeof(arr));
    }

    /** initialize to 'null' */
    void clear() {
        std::memset(_data, 0, kOIDSize);
    }

    int compare(const OID& other) const {
        return memcmp(_data, other._data, kOIDSize);
    }

    /** @return the object ID output as 24 hex digits */
    std::string toString() const;
    /** @return the random/sequential part of the object ID as 6 hex digits */
    std::string toIncString() const;

    static OID gen() {
        OID o((no_initialize_tag()));
        o.init();
        return o;
    }

    MONGO_STATIC_ASSERT_MSG(sizeof(int64_t) == kInstanceUniqueSize + kIncrementSize,
                            "size of term must be size of instance unique + increment");

    // Return OID initialized with a 8 byte term id and max Timestamp. Used for ElectionID.
    static OID fromTerm(int64_t term) {
        OID result;
        result.initFromTermNumber(term);
        return result;
    }

    // Caller must ensure that the buffer is valid for kOIDSize bytes.
    // this is templated because some places use unsigned char vs signed char
    template <typename T>
    static OID from(T* buf) {
        OID o((no_initialize_tag()));
        std::memcpy(o._data, buf, OID::kOIDSize);
        return o;
    }

    static OID max() {
        OID o((no_initialize_tag()));
        std::memset(o._data, 0xFF, kOIDSize);
        return o;
    }

    /**
     * This method creates and initializes an OID from a string,
     * returning a bad Status on failure.
     */
    static StatusWith<OID> parse(std::string_view input);

    /**
     * This method creates and initializes an OID from a string, throwing a BadValue exception if
     * the string is not a valid OID.
     */
    static OID createFromString(std::string_view input) {
        return uassertStatusOK(parse(input));
    }

    /** sets the contents to a new oid / randomized value */
    void init();

    /** init from a 24 char hex std::string */
    void init(std::string_view s);

    /** Set to the min/max OID that could be generated at given timestamp. */
    void init(Date_t date, bool max = false);

    /**
     * Sets the contents to contain the leading max Timestamp (0x7FFFFFFF)
     * followed by an big endian 8 byte term id
     */
    void initFromTermNumber(int64_t term);

    /**
     * Sets the contents to contain a leading zero Timestamp followed by a big endian 8 byte
     * int64 value.
     */
    void initFromInt64(int64_t val);

    time_t asTimeT() const;
    Date_t asDateT() const {
        return Date_t::fromMillisSinceEpoch(asTimeT() * 1000LL);
    }

    // True iff the OID is not empty
    bool isSet() const {
        return compare(OID()) != 0;
    }

    /**
     * Functor compatible with std::hash for std::unordered_{map,set}
     * Warning: The hash function is subject to change. Do not use in cases where hashes need
     *          to be consistent across versions.
     */
    struct Hasher {
        size_t operator()(const OID& oid) const;
    };

    /**
     * this is not consistent
     * do not store on disk
     */
    void hash_combine(size_t& seed) const;

    /**
     * Hash function compatible with absl::Hash for absl::unordered_{map,set}
     */
    template <typename H>
    friend H AbslHashValue(H h, const OID& oid) {
        const auto& d = oid._data;
        return H::combine(std::move(h), std::string_view{d, sizeof(d)});
    }

    /** call this after a fork to update the process id */
    static void justForked();

    static unsigned getMachineId();  // used by the 'features' command
    static void regenMachineId();

    // Timestamp is 4 bytes so we just use int32_t
    typedef int32_t Timestamp;

    // Wrappers so we can return stuff by value.
    struct InstanceUnique {
        static InstanceUnique generate(SecureRandom& entropy);
        uint8_t bytes[kInstanceUniqueSize];
    };

    struct Increment {
    public:
        static Increment next();
        uint8_t bytes[kIncrementSize];
    };

    void setTimestamp(Timestamp timestamp);
    void setInstanceUnique(InstanceUnique unique);
    void setIncrement(Increment inc);

    Timestamp getTimestamp() const;
    InstanceUnique getInstanceUnique() const;
    Increment getIncrement() const;

    ConstDataView view() const {
        return ConstDataView(_data);
    }

    ConstDataRange toCDR() const {
        return ConstDataRange(_data);
    }

private:
    // Internal mutable view
    DataView _view() {
        return DataView(_data);
    }

    // When we are going to immediately overwrite the bytes, there is no point in zero
    // initializing the data first.
    struct no_initialize_tag {};
    explicit OID(no_initialize_tag) {}

    char _data[kOIDSize];
};

inline std::ostream& operator<<(std::ostream& s, const OID& o) {
    return (s << o.toString());
}

inline StringBuilder& operator<<(StringBuilder& s, const OID& o) {
    return (s << o.toString());
}

/** Formatting mode for generating JSON from BSON.
    See <http://dochub.mongodb.org/core/mongodbextendedjson>
    for details.
*/
enum JsonStringFormat { ExtendedCanonicalV2_0_0, ExtendedRelaxedV2_0_0, LegacyStrict };

inline bool operator==(const OID& lhs, const OID& rhs) {
    return lhs.compare(rhs) == 0;
}
inline bool operator!=(const OID& lhs, const OID& rhs) {
    return lhs.compare(rhs) != 0;
}
inline bool operator<(const OID& lhs, const OID& rhs) {
    return lhs.compare(rhs) < 0;
}
inline bool operator<=(const OID& lhs, const OID& rhs) {
    return lhs.compare(rhs) <= 0;
}

}  // namespace mongo
