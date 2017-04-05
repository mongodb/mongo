/**
 *    Copyright (C) 2017 MongoDB, Inc.
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

#include "mongo/db/keys_collection_document.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/time_proof_service.h"

namespace mongo {

namespace {

const char kKeyIdFieldName[] = "_id";
const char kPurposeFieldName[] = "purpose";
const char kKeyFieldName[] = "key";
const char kExpiresAtFieldName[] = "expiresAt";

}  // namespace

StatusWith<KeysCollectionDocument> KeysCollectionDocument::fromBSON(const BSONObj& source) {
    long long keyId;
    Status status = bsonExtractIntegerField(source, kKeyIdFieldName, &keyId);
    if (!status.isOK()) {
        return status;
    }

    std::string purpose;
    status = bsonExtractStringField(source, kPurposeFieldName, &purpose);
    if (!status.isOK()) {
        return status;
    }

    // Extract BinData type signature hash and construct a SHA1Block instance from it.
    BSONElement keyElem;
    status = bsonExtractTypedField(source, kKeyFieldName, BinData, &keyElem);
    if (!status.isOK()) {
        return status;
    }

    int hashLength = 0;
    auto rawBinSignature = keyElem.binData(hashLength);
    BSONBinData proofBinData(rawBinSignature, hashLength, keyElem.binDataType());
    auto keyStatus = SHA1Block::fromBinData(proofBinData);
    if (!keyStatus.isOK()) {
        return keyStatus.getStatus();
    }

    Timestamp ts;
    status = bsonExtractTimestampField(source, kExpiresAtFieldName, &ts);
    if (!status.isOK()) {
        return status;
    }

    return KeysCollectionDocument(
        keyId, std::move(purpose), std::move(keyStatus.getValue()), LogicalTime(ts));
}

BSONObj KeysCollectionDocument::toBSON() const {
    BSONObjBuilder builder;

    builder.append(kKeyIdFieldName, _keyId);
    builder.append(kPurposeFieldName, _purpose);
    _key.appendAsBinData(builder, kKeyFieldName);
    _expiresAt.asTimestamp().append(builder.bb(), kExpiresAtFieldName);

    return builder.obj();
}

long long KeysCollectionDocument::getKeyId() const {
    return _keyId;
}

const std::string& KeysCollectionDocument::getPurpose() const {
    return _purpose;
}

const TimeProofService::Key& KeysCollectionDocument::getKey() const {
    return _key;
}

const LogicalTime& KeysCollectionDocument::getExpiresAt() const {
    return _expiresAt;
}

}  // namespace mongo
