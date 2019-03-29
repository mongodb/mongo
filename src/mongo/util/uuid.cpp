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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

namespace {

stdx::mutex uuidGenMutex;
auto uuidGen = SecureRandom::create();

}  // namespace

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

}  // namespace mongo
