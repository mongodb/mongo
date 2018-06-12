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

#include "mongo/db/pipeline/resume_token.h"

#include <boost/optional/optional_io.hpp>
#include <limits>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document_sources_gen.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/hex.h"

namespace mongo {
constexpr StringData ResumeToken::kDataFieldName;
constexpr StringData ResumeToken::kTypeBitsFieldName;

bool ResumeTokenData::operator==(const ResumeTokenData& other) const {
    return clusterTime == other.clusterTime &&
        (Value::compare(this->documentKey, other.documentKey, nullptr) == 0) && uuid == other.uuid;
}

std::ostream& operator<<(std::ostream& out, const ResumeTokenData& tokenData) {
    return out << "{clusterTime: " << tokenData.clusterTime.toString()
               << "  documentKey: " << tokenData.documentKey << "  uuid: " << tokenData.uuid << "}";
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
            _typeBits.missing() || (_typeBits.getType() == BSONType::BinData &&
                                    _typeBits.getBinData().type == BinDataGeneral));
}

// We encode the resume token as a KeyString with the sequence:
// clusterTime, version, applyOpsIndex, uuid, documentKey
// Only the clusterTime, version, and applyOpsIndex are required.
ResumeToken::ResumeToken(const ResumeTokenData& data) {
    BSONObjBuilder builder;
    builder.append("", data.clusterTime);
    builder.append("", data.version);
    builder.appendNumber("", data.applyOpsIndex);
    uassert(50788,
            "Unexpected resume token with a documentKey but no UUID",
            data.uuid || data.documentKey.missing());

    if (data.uuid) {
        data.uuid->appendToBuilder(&builder, "");
    }
    data.documentKey.addToBsonObj(&builder, "");
    auto keyObj = builder.obj();
    KeyString encodedToken(KeyString::Version::V1, keyObj, Ordering::make(BSONObj()));
    _hexKeyString = toHex(encodedToken.getBuffer(), encodedToken.getSize());
    const auto& typeBits = encodedToken.getTypeBits();
    if (!typeBits.isAllZeros())
        _typeBits = Value(
            BSONBinData(typeBits.getBuffer(), typeBits.getSize(), BinDataType::BinDataGeneral));
}

bool ResumeToken::operator==(const ResumeToken& other) const {
    // '_hexKeyString' is enough to determine equality. The type bits are used to unambiguously
    // re-construct the original data, but we do not expect any two resume tokens to have the same
    // data and different type bits, since that would imply they have (1) the same timestamp and (2)
    // the same documentKey (possibly different types). This should not be possible because
    // documents with the same documentKey should be on the same shard and therefore should have
    // different timestamps.
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
            isValidHex(_hexKeyString));

    BufBuilder hexDecodeBuf;  // Keep this in scope until we've decoded the bytes.
    fromHexString(_hexKeyString, &hexDecodeBuf);
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
    uassert(50795, "Invalid Resume Token: only supports version 0", result.version == 0);

    // Next comes the applyOps index.
    uassert(50793, "Resume Token does not contain applyOpsIndex", i.more());
    auto applyOpsElt = i.next();
    uassert(50855,
            "Resume Token applyOpsIndex is not an integer",
            applyOpsElt.type() == BSONType::NumberInt);
    const int applyOpsInd = applyOpsElt.numberInt();
    uassert(50794, "Invalid Resume Token: applyOpsIndex should be non-negative", applyOpsInd >= 0);
    result.applyOpsIndex = applyOpsInd;

    // The UUID and documentKey are not required.
    if (!i.more()) {
        return result;
    }

    // The UUID comes first, then the documentKey.
    result.uuid = uassertStatusOK(UUID::parse(i.next()));
    if (i.more()) {
        result.documentKey = Value(i.next());
    }
    uassert(40646, "invalid oversized resume token", !i.more());
    return result;
}

Document ResumeToken::toDocument() const {
    return Document{{kDataFieldName, _hexKeyString}, {kTypeBitsFieldName, _typeBits}};
}

ResumeToken ResumeToken::parse(const Document& resumeDoc) {
    return ResumeToken(resumeDoc);
}

}  // namespace mongo
