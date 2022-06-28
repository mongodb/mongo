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

#include "mongo/db/pipeline/resume_token.h"

#include <boost/optional/optional_io.hpp>
#include <limits>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/change_stream_helpers_legacy.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/hex.h"

namespace mongo {
constexpr StringData ResumeToken::kDataFieldName;
constexpr StringData ResumeToken::kTypeBitsFieldName;

ResumeTokenData::ResumeTokenData(Timestamp clusterTimeIn,
                                 int versionIn,
                                 size_t txnOpIndexIn,
                                 const boost::optional<UUID>& uuidIn,
                                 StringData opType,
                                 Value documentKey,
                                 Value opDescription)
    : clusterTime(clusterTimeIn), version(versionIn), txnOpIndex(txnOpIndexIn), uuid(uuidIn) {
    tassert(6280100,
            "both documentKey and operationDescription cannot be present for an event",
            documentKey.missing() || opDescription.missing());

    // For v1 classic change events, the eventIdentifier is always the documentKey, even if missing.
    if (change_stream_legacy::kClassicOperationTypes.count(opType) && version <= 1) {
        eventIdentifier = documentKey;
        return;
    }

    // If we are here, then this is either a v2 classic event or an expanded event. In both cases,
    // the resume token is the operationType plus the documentKey or operationDescription.
    auto opDescOrDocKey = documentKey.missing()
        ? std::make_pair("operationDescription"_sd, opDescription)
        : std::make_pair("documentKey"_sd, documentKey);

    eventIdentifier = Value(Document{{"operationType"_sd, opType}, std::move(opDescOrDocKey)});
};

bool ResumeTokenData::operator==(const ResumeTokenData& other) const {
    return clusterTime == other.clusterTime && version == other.version &&
        tokenType == other.tokenType && txnOpIndex == other.txnOpIndex &&
        fromInvalidate == other.fromInvalidate && uuid == other.uuid &&
        (Value::compare(this->eventIdentifier, other.eventIdentifier, nullptr) == 0);
}

std::ostream& operator<<(std::ostream& out, const ResumeTokenData& tokenData) {
    out << "{clusterTime: " << tokenData.clusterTime.toString();
    out << ", version: " << tokenData.version;
    if (tokenData.version > 0) {
        out << ", tokenType: " << tokenData.tokenType;
    }
    out << ", txnOpIndex: " << tokenData.txnOpIndex;
    if (tokenData.version > 0) {
        out << ", fromInvalidate: " << static_cast<bool>(tokenData.fromInvalidate);
    }
    out << ", uuid: " << tokenData.uuid;
    out << ", eventIdentifier: " << tokenData.eventIdentifier << "}";
    return out;
}

ResumeToken::ResumeToken(const Document& resumeDoc) {
    auto dataVal = resumeDoc[kDataFieldName];
    uassert(40647,
            str::stream()
                << "Bad resume token: _data of missing or of wrong type. Expected string, got "
                << resumeDoc.toString(),
            dataVal.getType() == BSONType::String);
    _hexKeyString = dataVal.getString();
    _typeBits = resumeDoc[kTypeBitsFieldName];
    uassert(40648,
            str::stream() << "Bad resume token: _typeBits of wrong type " << resumeDoc.toString(),
            _typeBits.missing() ||
                (_typeBits.getType() == BSONType::BinData &&
                 _typeBits.getBinData().type == BinDataGeneral));
}

// We encode the resume token as a KeyString with the sequence:
// clusterTime, version, txnOpIndex, fromInvalidate, uuid, eventIdentifier Only the clusterTime,
// version, txnOpIndex, and fromInvalidate are required.
ResumeToken::ResumeToken(const ResumeTokenData& data) {
    BSONObjBuilder builder;
    builder.append("", data.clusterTime);
    builder.append("", data.version);
    if (data.version >= 1) {
        builder.appendNumber("", data.tokenType);
    }
    builder.appendNumber("", static_cast<long long>(data.txnOpIndex));
    if (data.version >= 1) {
        builder.appendBool("", data.fromInvalidate);
    }

    // High water mark tokens only populate the clusterTime, version, and tokenType fields.
    uassert(6189505,
            "Invalid high water mark token",
            !(data.tokenType == ResumeTokenData::TokenType::kHighWaterMarkToken &&
              (data.txnOpIndex > 0 || data.fromInvalidate || data.uuid ||
               !data.eventIdentifier.missing())));

    // From v2 onwards, tokens may have an eventIdentifier but no UUID.
    uassert(50788,
            "Unexpected resume token with a eventIdentifier but no UUID",
            data.uuid || data.eventIdentifier.missing() || data.version >= 2);

    // From v2 onwards, all non-high-water-mark tokens must have an eventIdentifier.
    uassert(6189502,
            "Expected an eventIdentifier for an event resume token",
            !(data.tokenType == ResumeTokenData::TokenType::kEventToken &&
              data.eventIdentifier.missing() && data.version >= 2));

    // From v2 onwards, a missing UUID is encoded as explicitly null.
    if (data.uuid) {
        data.uuid->appendToBuilder(&builder, "");
    } else if (data.version >= 2) {
        builder.appendNull("");
    }
    data.eventIdentifier.addToBsonObj(&builder, "");

    auto keyObj = builder.obj();
    KeyString::Builder encodedToken(KeyString::Version::V1, keyObj, Ordering::make(BSONObj()));
    _hexKeyString = hexblob::encode(encodedToken.getBuffer(), encodedToken.getSize());
    const auto& typeBits = encodedToken.getTypeBits();
    if (!typeBits.isAllZeros())
        _typeBits = Value(
            BSONBinData(typeBits.getBuffer(), typeBits.getSize(), BinDataType::BinDataGeneral));
}

bool ResumeToken::operator==(const ResumeToken& other) const {
    // '_hexKeyString' is enough to determine equality. The type bits are used to unambiguously
    // re-construct the original data, but we do not expect any two resume tokens to have the same
    // data and different type bits, since that would imply they have (1) the same timestamp and (2)
    // the same eventIdentifier fields and values, but with different types. Change events with the
    // same eventIdentifier are either (1) on the same shard in the case of CRUD events, which
    // implies that they must have different timestamps; or (2) refer to the same logical event on
    // different shards, in the case of non-CRUD events.
    return _hexKeyString == other._hexKeyString;
}

ResumeTokenData ResumeToken::getData() const {
    KeyString::TypeBits typeBits(KeyString::Version::V1);
    if (!_typeBits.missing()) {
        BSONBinData typeBitsBinData = _typeBits.getBinData();
        BufReader typeBitsReader(typeBitsBinData.data, typeBitsBinData.length);
        typeBits.resetFromBuffer(&typeBitsReader);
    }

    uassert(ErrorCodes::FailedToParse,
            "resume token string was not a valid hex string",
            hexblob::validate(_hexKeyString));

    BufBuilder hexDecodeBuf;  // Keep this in scope until we've decoded the bytes.
    hexblob::decode(_hexKeyString, &hexDecodeBuf);
    BSONBinData keyStringBinData =
        BSONBinData(hexDecodeBuf.buf(), hexDecodeBuf.len(), BinDataType::BinDataGeneral);
    auto internalBson = KeyString::toBsonSafe(static_cast<const char*>(keyStringBinData.data),
                                              keyStringBinData.length,
                                              Ordering::make(BSONObj()),
                                              typeBits);

    BSONObjIterator i(internalBson);
    ResumeTokenData result;
    uassert(40649, "invalid empty resume token", i.more());
    result.clusterTime = i.next().timestamp();
    // Next comes the resume token version.
    uassert(50796, "Resume Token does not contain version", i.more());
    auto versionElt = i.next();
    uassert(50854,
            "Invalid resume token: wrong type for version",
            versionElt.type() == BSONType::NumberInt);
    result.version = versionElt.numberInt();
    uassert(50795,
            "Invalid Resume Token: only supports version 0, 1 and 2",
            result.version == 0 || result.version == 1 || result.version == 2);

    if (result.version >= 1) {
        // The 'tokenType' field was added in version 1 and is not present in v0 tokens.
        uassert(51055, "Resume Token does not contain tokenType", i.more());
        auto tokenType = i.next();
        uassert(51056,
                "Resume Token tokenType is not an int.",
                tokenType.type() == BSONType::NumberInt);
        auto typeInt = tokenType.numberInt();
        uassert(51057,
                str::stream() << "Token type " << typeInt << " not recognized",
                typeInt == ResumeTokenData::TokenType::kEventToken ||
                    typeInt == ResumeTokenData::TokenType::kHighWaterMarkToken);
        result.tokenType = static_cast<ResumeTokenData::TokenType>(typeInt);
    }

    // Next comes the txnOpIndex value.
    uassert(50793, "Resume Token does not contain txnOpIndex", i.more());
    auto txnOpIndexElt = i.next();
    uassert(50855,
            "Resume Token txnOpIndex is not an integer",
            txnOpIndexElt.type() == BSONType::NumberInt);
    const int txnOpIndexInd = txnOpIndexElt.numberInt();
    uassert(50794, "Invalid Resume Token: txnOpIndex should be non-negative", txnOpIndexInd >= 0);
    result.txnOpIndex = txnOpIndexInd;

    if (result.version >= 1) {
        // The 'fromInvalidate' bool was added in version 1 resume tokens. We don't expect to see it
        // on version 0 tokens. After this bool, the remaining fields should be the same.
        uassert(50872, "Resume Token does not contain fromInvalidate", i.more());
        auto fromInvalidate = i.next();
        uassert(50870,
                "Resume Token fromInvalidate is not a boolean.",
                fromInvalidate.type() == BSONType::Bool);
        result.fromInvalidate = ResumeTokenData::FromInvalidate(fromInvalidate.boolean());
    }

    // The UUID and eventIdentifier are not required for token versions <= 1.
    if (!i.more()) {
        uassert(6189500, "Expected UUID or null", result.version <= 1);
        return result;
    }

    // The UUID comes first, then eventIdentifier. From v2 onwards, UUID may be explicitly null.
    if (auto uuidElem = i.next(); uuidElem.type() != BSONType::jstNULL) {
        result.uuid = uassertStatusOK(UUID::parse(uuidElem));
    }

    // High water mark tokens never have an eventIdentifier.
    uassert(6189504,
            "Invalid high water mark token",
            !(result.tokenType == ResumeTokenData::TokenType::kHighWaterMarkToken && i.more()));

    // From v2 onwards, all non-high-water-mark tokens must have an eventIdentifier.
    if (!i.more()) {
        uassert(
            6189501,
            "Expected an eventIdentifier for an event resume token",
            !(result.tokenType == ResumeTokenData::TokenType::kEventToken && result.version >= 2));
        return result;
    }

    result.eventIdentifier = Value(i.next());
    uassert(6189503,
            "Resume Token eventIdentifier is not an object",
            result.eventIdentifier.getType() == BSONType::Object);

    uassert(40646, "invalid oversized resume token", !i.more());
    return result;
}

Document ResumeToken::toDocument() const {
    return Document{{kDataFieldName, _hexKeyString}, {kTypeBitsFieldName, _typeBits}};
}

ResumeToken ResumeToken::parse(const Document& resumeDoc) {
    return ResumeToken(resumeDoc);
}

ResumeTokenData ResumeToken::makeHighWaterMarkTokenData(Timestamp clusterTime, int version) {
    ResumeTokenData tokenData;
    tokenData.version = version;
    tokenData.clusterTime = clusterTime;
    tokenData.tokenType = ResumeTokenData::kHighWaterMarkToken;
    return tokenData;
}

ResumeToken ResumeToken::makeHighWaterMarkToken(Timestamp clusterTime, int version) {
    return ResumeToken(makeHighWaterMarkTokenData(clusterTime, version));
}

bool ResumeToken::isHighWaterMarkToken(const ResumeTokenData& tokenData) {
    return tokenData == makeHighWaterMarkTokenData(tokenData.clusterTime, tokenData.version);
}

}  // namespace mongo
