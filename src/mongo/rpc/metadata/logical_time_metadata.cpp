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

#include "mongo/rpc/metadata/logical_time_metadata.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"

namespace mongo {
namespace rpc {

namespace {

const char kClusterTimeFieldName[] = "clusterTime";
const char kSignatureFieldName[] = "signature";
const char kSignatureHashFieldName[] = "hash";
const char kSignatureKeyIdFieldName[] = "keyId";

}  // unnamed namespace


LogicalTimeMetadata::LogicalTimeMetadata(SignedLogicalTime time) : _clusterTime(std::move(time)) {}

StatusWith<LogicalTimeMetadata> LogicalTimeMetadata::readFromMetadata(const BSONObj& metadata) {
    return readFromMetadata(metadata.getField(fieldName()));
}

StatusWith<LogicalTimeMetadata> LogicalTimeMetadata::readFromMetadata(
    const BSONElement& metadataElem) {
    if (metadataElem.eoo()) {
        return LogicalTimeMetadata();
    }

    const auto& obj = metadataElem.Obj();

    Timestamp ts;
    Status status = bsonExtractTimestampField(obj, kClusterTimeFieldName, &ts);
    if (!status.isOK()) {
        return status;
    }

    BSONElement signatureElem;
    status = bsonExtractTypedField(obj, kSignatureFieldName, Object, &signatureElem);
    if (!status.isOK()) {
        return status;
    }

    const auto& signatureObj = signatureElem.Obj();

    // Extract BinData type signature hash and construct a SHA1Block instance from it.
    BSONElement hashElem;
    status = bsonExtractTypedField(signatureObj, kSignatureHashFieldName, BinData, &hashElem);
    if (!status.isOK()) {
        return status;
    }

    int hashLength = 0;
    auto rawBinSignature = hashElem.binData(hashLength);
    BSONBinData proofBinData(rawBinSignature, hashLength, hashElem.binDataType());
    auto proofStatus = SHA1Block::fromBinData(proofBinData);

    if (!proofStatus.isOK()) {
        return proofStatus.getStatus();
    }

    long long keyId;
    status = bsonExtractIntegerField(signatureObj, kSignatureKeyIdFieldName, &keyId);
    if (!status.isOK()) {
        return status;
    }

    return LogicalTimeMetadata(
        SignedLogicalTime(LogicalTime(ts), std::move(proofStatus.getValue()), keyId));
}

void LogicalTimeMetadata::writeToMetadata(BSONObjBuilder* metadataBuilder) const {
    BSONObjBuilder subObjBuilder(metadataBuilder->subobjStart(fieldName()));
    _clusterTime.getTime().asTimestamp().append(subObjBuilder.bb(), kClusterTimeFieldName);

    BSONObjBuilder signatureObjBuilder(subObjBuilder.subobjStart(kSignatureFieldName));
    // Cluster time metadata is only written when the LogicalTimeValidator is set, which
    // means the cluster time should always have a proof.
    invariant(_clusterTime.getProof());
    _clusterTime.getProof()->appendAsBinData(signatureObjBuilder, kSignatureHashFieldName);
    signatureObjBuilder.append(kSignatureKeyIdFieldName, _clusterTime.getKeyId());
    signatureObjBuilder.doneFast();

    subObjBuilder.doneFast();
}

const SignedLogicalTime& LogicalTimeMetadata::getSignedTime() const {
    return _clusterTime;
}

}  // namespace rpc
}  // namespace mongo
