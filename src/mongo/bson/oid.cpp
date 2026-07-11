// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/oid.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/platform/atomic.h"
#include "mongo/platform/random.h"
#include "mongo/util/hex.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <ctime>
#include <limits>
#include <memory>
#include <string_view>

#include <boost/functional/hash.hpp>
#include <boost/move/utility_core.hpp>

namespace mongo {

namespace {
std::unique_ptr<Atomic<int64_t>> counter;

const std::size_t kTimestampOffset = 0;
const std::size_t kInstanceUniqueOffset = kTimestampOffset + OID::kTimestampSize;
const std::size_t kIncrementOffset = kInstanceUniqueOffset + OID::kInstanceUniqueSize;
OID::InstanceUnique _instanceUnique;

bool isHexit(char c) {
    return ((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f')) || ((c >= 'A') && (c <= 'F'));
}

}  // namespace

MONGO_INITIALIZER_GENERAL(OIDGeneration, (), ("default"))
(InitializerContext* context) {
    SecureRandom entropy;
    counter = std::make_unique<Atomic<int64_t>>(entropy.nextInt64());
    _instanceUnique = OID::InstanceUnique::generate(entropy);
}

OID::Increment OID::Increment::next() {
    uint64_t nextCtr = counter->fetchAndAdd(1);
    OID::Increment incr;

    incr.bytes[0] = uint8_t(nextCtr >> 16);
    incr.bytes[1] = uint8_t(nextCtr >> 8);
    incr.bytes[2] = uint8_t(nextCtr);

    return incr;
}

OID::InstanceUnique OID::InstanceUnique::generate(SecureRandom& entropy) {
    OID::InstanceUnique u;
    entropy.fill(u.bytes, kInstanceUniqueSize);
    return u;
}

void OID::setTimestamp(const OID::Timestamp timestamp) {
    _view().write<BigEndian<Timestamp>>(timestamp, kTimestampOffset);
}

void OID::setInstanceUnique(const OID::InstanceUnique unique) {
    // Byte order doesn't matter here
    _view().write<InstanceUnique>(unique, kInstanceUniqueOffset);
}

void OID::setIncrement(const OID::Increment inc) {
    _view().write<Increment>(inc, kIncrementOffset);
}

OID::Timestamp OID::getTimestamp() const {
    return view().read<BigEndian<Timestamp>>(kTimestampOffset);
}

OID::InstanceUnique OID::getInstanceUnique() const {
    // Byte order doesn't matter here
    return view().read<InstanceUnique>(kInstanceUniqueOffset);
}

OID::Increment OID::getIncrement() const {
    return view().read<Increment>(kIncrementOffset);
}

void OID::hash_combine(size_t& seed) const {
    uint32_t v;
    for (int i = 0; i != kOIDSize; i += sizeof(uint32_t)) {
        memcpy(&v, _data + i, sizeof(uint32_t));
        boost::hash_combine(seed, v);
    }
}

size_t OID::Hasher::operator()(const OID& oid) const {
    size_t seed = 0;
    oid.hash_combine(seed);
    return seed;
}

void OID::regenMachineId() {
    SecureRandom entropy;
    _instanceUnique = InstanceUnique::generate(entropy);
}

unsigned OID::getMachineId() {
    uint32_t ret = 0;
    std::memcpy(&ret, _instanceUnique.bytes, sizeof(uint32_t));
    return ret;
}

void OID::justForked() {
    regenMachineId();
}

void OID::init() {
    // each set* method handles endianness
    setTimestamp(time(nullptr));
    setInstanceUnique(_instanceUnique);
    setIncrement(Increment::next());
}

void OID::initFromTermNumber(int64_t term) {
    // Each set* method handles endianness.
    // Set max timestamp because the drivers compare ElectionId's to determine valid new primaries,
    // and we want ElectionId's with terms to supercede ones without terms.
    setTimestamp(std::numeric_limits<Timestamp>::max());
    _view().write<BigEndian<int64_t>>(term, kInstanceUniqueOffset);
}

void OID::initFromInt64(int64_t val) {
    // Clear the top 4 bytes.
    setTimestamp(0);
    _view().write<BigEndian<int64_t>>(val, kInstanceUniqueOffset);
}

void OID::init(std::string_view s) {
    MONGO_verify(s.size() == (2 * kOIDSize));
    std::string blob = hexblob::decode(s.substr(0, 2 * kOIDSize));
    std::copy(blob.begin(), blob.end(), _data);
}

StatusWith<OID> OID::parse(std::string_view input) {
    if (input.size() != (2 * kOIDSize)) {
        return {ErrorCodes::BadValue,
                str::stream() << "Invalid string length for parsing to OID, expected "
                              << (2 * kOIDSize) << " but found " << input.size()};
    }

    for (char c : input) {
        if (!isHexit(c)) {
            return {ErrorCodes::BadValue,
                    str::stream() << "Invalid character found in hex string: " << c};
        }
    }

    return OID(input);
}

void OID::init(Date_t date, bool max) {
    setTimestamp(uint32_t(date.toMillisSinceEpoch() / 1000));
    uint64_t rest = max ? std::numeric_limits<uint64_t>::max() : 0u;
    std::memcpy(_view().view(kInstanceUniqueOffset), &rest, kInstanceUniqueSize + kIncrementSize);
}

time_t OID::asTimeT() const {
    return getTimestamp();
}

std::string OID::toString() const {
    return hexblob::encodeLower(_data, kOIDSize);
}

std::string OID::toIncString() const {
    return hexblob::encodeLower(getIncrement().bytes, kIncrementSize);
}

}  // namespace mongo
