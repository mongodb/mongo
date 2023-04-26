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

#include "mongo/platform/basic.h"

#include "mongo/util/uuid.h"

#include <algorithm>
#include <fmt/format.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/random.h"
#include "mongo/util/ctype.h"
#include "mongo/util/hex.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/synchronized_value.h"

namespace mongo {

namespace {

using namespace fmt::literals;

synchronized_value<SecureRandom>& uuidGen() {
    static StaticImmortal<synchronized_value<SecureRandom>> uuidGen;
    return uuidGen.value();
}

}  // namespace

StatusWith<UUID> UUID::parse(BSONElement from) {
    try {
        return UUID{from.uuid()};
    } catch (const AssertionException& e) {
        return e.toStatus();
    }
}

StatusWith<UUID> UUID::parse(StringData s) {
    if (!isUUIDString(s)) {
        return {ErrorCodes::InvalidUUID, "Invalid UUID string: {}"_format(s)};
    }

    UUIDStorage uuid;

    // 4 Octets - 2 Octets - 2 Octets - 2 Octets - 6 Octets
    int j = 0;
    for (int i = 0; i < UUID::kNumBytes; i++) {
        // Skip hyphens
        if (s[j] == '-')
            j++;

        uuid[i] = hexblob::decodePair(s.substr(j, 2));
        j += 2;
    }

    return UUID{std::move(uuid)};
}

UUID UUID::parse(const BSONObj& obj) {
    auto res = parse(obj.getField("uuid"));
    uassert(40566, res.getStatus().reason(), res.isOK());
    return res.getValue();
}

bool UUID::isUUIDString(StringData s) {
    static constexpr auto pat = "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"_sd;
    return s.size() == pat.size() &&
        std::mismatch(s.begin(), s.end(), pat.begin(), [](char a, char b) {
            return b == 'x' ? ctype::isXdigit(a) : a == b;
        }).first == s.end();
}

bool UUID::isRFC4122v4() const {
    return (_uuid[6] & ~0x0f) == 0x40 && (_uuid[8] & ~0x3f) == 0x80;  // See RFC 4122, section 4.4.
}

UUID UUID::gen() {
    UUIDStorage randomBytes;
    uuidGen()->fill(&randomBytes, sizeof(randomBytes));

    // Set version in high 4 bits of byte 6 and variant in high 2 bits of byte 8, see RFC 4122,
    // section 4.1.1, 4.1.2 and 4.1.3.
    randomBytes[6] &= 0x0f;
    randomBytes[6] |= 0x40;  // v4
    randomBytes[8] &= 0x3f;
    randomBytes[8] |= 0x80;  // Randomly assigned

    return UUID{randomBytes};
}

void UUID::appendToBuilder(BSONObjBuilder* builder, StringData name) const {
    builder->appendBinData(name, sizeof(UUIDStorage), BinDataType::newUUID, &_uuid);
}

void UUID::appendToArrayBuilder(BSONArrayBuilder* builder) const {
    builder->appendBinData(sizeof(UUIDStorage), BinDataType::newUUID, &_uuid);
}

BSONObj UUID::toBSON() const {
    BSONObjBuilder builder;
    appendToBuilder(&builder, "uuid");
    return builder.obj();
}

std::string UUID::toString() const {
    return "{}-{}-{}-{}-{}"_format(hexblob::encodeLower(&_uuid[0], 4),
                                   hexblob::encodeLower(&_uuid[4], 2),
                                   hexblob::encodeLower(&_uuid[6], 2),
                                   hexblob::encodeLower(&_uuid[8], 2),
                                   hexblob::encodeLower(&_uuid[10], 6));
}

template <>
BSONObjBuilder& BSONObjBuilderValueStream::operator<<<UUID>(UUID value) {
    value.appendToBuilder(_builder, _fieldName);
    _fieldName = StringData();
    return *_builder;
}

}  // namespace mongo
