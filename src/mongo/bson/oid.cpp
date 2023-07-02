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

#include <algorithm>
#include <boost/container_hash/extensions.hpp>
#include <ctime>
#include <limits>
#include <memory>

#include <boost/move/utility_core.hpp>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/random.h"
#include "mongo/util/hex.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {
std::unique_ptr<AtomicWord<int64_t>> counter;

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
    counter = std::make_unique<AtomicWord<int64_t>>(entropy.nextInt64());
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

void OID::init(StringData s) {
    MONGO_verify(s.size() == (2 * kOIDSize));
    std::string blob = hexblob::decode(s.substr(0, 2 * kOIDSize));
    std::copy(blob.begin(), blob.end(), _data);
}

StatusWith<OID> OID::parse(StringData input) {
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
