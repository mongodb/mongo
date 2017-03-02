/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
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

#include "collection_uuid.h"

#include "mongo/db/server_parameters.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

bool enableCollectionUUIDs = false;
ExportedServerParameter<bool, ServerParameterType::kStartupOnly> enableCollectionUUIDsParameter(
    ServerParameterSet::getGlobal(), "enableCollectionUUIDs", &enableCollectionUUIDs);

namespace {
stdx::mutex uuidGenMutex;
auto uuidGen = SecureRandom::create();
}  // namespace

// static
CollectionUUID CollectionUUID::generateSecureRandomUUID() {
    stdx::unique_lock<stdx::mutex> lock(uuidGenMutex);
    int64_t randomWords[2] = {uuidGen->nextInt64(), uuidGen->nextInt64()};
    UUID randomBytes;
    memcpy(&randomBytes, randomWords, sizeof(randomBytes));
    // Set version in high 4 bits of byte 6 and variant in high 2 bits of byte 8, see RFC 4122,
    // section 4.1.1, 4.1.2 and 4.1.3.
    randomBytes[6] &= 0x0f;
    randomBytes[6] |= 0x40;  // v4
    randomBytes[8] &= 0x3f;
    randomBytes[8] |= 0x80;  // Randomly assigned
    return CollectionUUID{randomBytes};
}
}  // namespace mongo
