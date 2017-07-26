/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <regex>

#include "mongo/util/uuid.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/hex.h"

namespace mongo {

namespace {

stdx::mutex uuidGenMutex;
auto uuidGen = SecureRandom::create();

// Regex to match valid version 4 UUIDs with variant bits set
std::regex uuidRegex("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}",
                     std::regex::optimize);

}  // namespace

StatusWith<UUID> UUID::parse(BSONElement from) {
    try {
        return UUID{from.uuid()};
    } catch (const UserException& e) {
        return e.toStatus();
    }
}

StatusWith<UUID> UUID::parse(const std::string& s) {
    if (!isUUIDString(s)) {
        return {ErrorCodes::InvalidUUID, "Invalid UUID string: " + s};
    }

    UUIDStorage uuid;

    // 4 Octets - 2 Octets - 2 Octets - 2 Octets - 6 Octets
    int j = 0;
    for (int i = 0; i < UUID::kNumBytes; i++) {
        // Skip hyphens
        if (s[j] == '-')
            j++;

        char high = s[j++];
        char low = s[j++];

        uuid[i] = ((fromHex(high) << 4) | fromHex(low));
    }

    return UUID{std::move(uuid)};
}

UUID UUID::parse(const BSONObj& obj) {
    auto res = parse(obj.getField("uuid"));
    uassert(40566, res.getStatus().reason(), res.isOK());
    return res.getValue();
}

bool UUID::isUUIDString(const std::string& s) {
    return std::regex_match(s, uuidRegex);
}

bool UUID::isRFC4122v4() const {
    return (_uuid[6] & ~0x0f) == 0x40 && (_uuid[8] & ~0x3f) == 0x80;  // See RFC 4122, section 4.4.
}

UUID UUID::gen() {
    int64_t randomWords[2];

    {
        stdx::lock_guard<stdx::mutex> lk(uuidGenMutex);

        // Generate 128 random bits
        randomWords[0] = uuidGen->nextInt64();
        randomWords[1] = uuidGen->nextInt64();
    }

    UUIDStorage randomBytes;
    memcpy(&randomBytes, randomWords, sizeof(randomBytes));

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

BSONObj UUID::toBSON() const {
    BSONObjBuilder builder;
    appendToBuilder(&builder, "uuid");
    return builder.obj();
}

std::string UUID::toString() const {
    StringBuilder ss;

    // 4 Octets - 2 Octets - 2 Octets - 2 Octets - 6 Octets
    ss << toHexLower(&_uuid[0], 4);
    ss << "-";
    ss << toHexLower(&_uuid[4], 2);
    ss << "-";
    ss << toHexLower(&_uuid[6], 2);
    ss << "-";
    ss << toHexLower(&_uuid[8], 2);
    ss << "-";
    ss << toHexLower(&_uuid[10], 6);

    return ss.str();
}

template <>
BSONObjBuilder& BSONObjBuilderValueStream::operator<<<UUID>(UUID value) {
    value.appendToBuilder(_builder, _fieldName);
    _fieldName = StringData();
    return *_builder;
}

}  // namespace mongo
