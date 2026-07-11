// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/uuid.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/hex.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/synchronized_value.h"

#include <algorithm>
#include <new>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>

namespace mongo {
using namespace std::literals::string_view_literals;

namespace {

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

StatusWith<UUID> UUID::parse(std::string_view s) {
    if (!isUUIDString(s)) {
        return {ErrorCodes::InvalidUUID, fmt::format("Invalid UUID string: {}", s)};
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

bool UUID::isUUIDString(std::string_view s) {
    static constexpr auto pat = "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"sv;
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

void UUID::appendToBuilder(BSONObjBuilder* builder, std::string_view name) const {
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
    return fmt::format("{}-{}-{}-{}-{}",
                       hexblob::encodeLower(&_uuid[0], 4),
                       hexblob::encodeLower(&_uuid[4], 2),
                       hexblob::encodeLower(&_uuid[6], 2),
                       hexblob::encodeLower(&_uuid[8], 2),
                       hexblob::encodeLower(&_uuid[10], 6));
}

}  // namespace mongo
