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

#include "mongo/db/pipeline/resume_token.h"

#include <boost/optional/optional_io.hpp>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document_sources_gen.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/db/storage/key_string.h"

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
    _keyStringData = resumeDoc[kDataFieldName];
    _typeBits = resumeDoc[kTypeBitsFieldName];
    uassert(40647,
            str::stream() << "Bad resume token: _data of missing or of wrong type"
                          << resumeDoc.toString(),
            _keyStringData.getType() == BinData &&
                _keyStringData.getBinData().type == BinDataGeneral);
    uassert(40648,
            str::stream() << "Bad resume token: _typeBits of wrong type" << resumeDoc.toString(),
            _typeBits.missing() ||
                (_typeBits.getType() == BinData && _typeBits.getBinData().type == BinDataGeneral));
}

// We encode the resume token as a KeyString with the sequence: clusterTime, documentKey, uuid.
// Only the clusterTime is required.
ResumeToken::ResumeToken(const ResumeTokenData& data) {
    BSONObjBuilder builder;
    builder.append("", data.clusterTime);
    data.documentKey.addToBsonObj(&builder, "");
    if (data.uuid) {
        if (data.documentKey.missing()) {
            // Never allow a missing document key with a UUID present, as that will mess up
            // the field order.
            builder.appendNull("");
        }
        data.uuid->appendToBuilder(&builder, "");
    }
    auto keyObj = builder.obj();
    KeyString encodedToken(KeyString::Version::V1, keyObj, Ordering::make(BSONObj()));
    _keyStringData = Value(
        BSONBinData(encodedToken.getBuffer(), encodedToken.getSize(), BinDataType::BinDataGeneral));
    const auto& typeBits = encodedToken.getTypeBits();
    if (!typeBits.isAllZeros())
        _typeBits = Value(
            BSONBinData(typeBits.getBuffer(), typeBits.getSize(), BinDataType::BinDataGeneral));
}

ResumeTokenData ResumeToken::getData() const {
    KeyString::TypeBits typeBits(KeyString::Version::V1);
    if (!_typeBits.missing()) {
        BSONBinData typeBitsBinData = _typeBits.getBinData();
        BufReader typeBitsReader(typeBitsBinData.data, typeBitsBinData.length);
        typeBits.resetFromBuffer(&typeBitsReader);
    }

    BSONBinData keyStringBinData = _keyStringData.getBinData();
    auto internalBson = KeyString::toBsonSafe(static_cast<const char*>(keyStringBinData.data),
                                              keyStringBinData.length,
                                              Ordering::make(BSONObj()),
                                              typeBits);

    BSONObjIterator i(internalBson);
    ResumeTokenData result;
    uassert(40649, "invalid empty resume token", i.more());
    result.clusterTime = i.next().timestamp();
    if (i.more())
        result.documentKey = Value(i.next());
    if (i.more())
        result.uuid = uassertStatusOK(UUID::parse(i.next()));
    uassert(40646, "invalid oversized resume token", !i.more());
    return result;
}

int ResumeToken::compare(const ResumeToken& other) const {
    BSONBinData thisData = _keyStringData.getBinData();
    BSONBinData otherData = other._keyStringData.getBinData();
    return StringData(static_cast<const char*>(thisData.data), thisData.length)
        .compare(StringData(static_cast<const char*>(otherData.data), otherData.length));
}

bool ResumeToken::operator==(const ResumeToken& other) const {
    return compare(other) == 0;
}

bool ResumeToken::operator!=(const ResumeToken& other) const {
    return compare(other) != 0;
}

bool ResumeToken::operator<(const ResumeToken& other) const {
    return compare(other) < 0;
}

bool ResumeToken::operator<=(const ResumeToken& other) const {
    return compare(other) <= 0;
}

bool ResumeToken::operator>(const ResumeToken& other) const {
    return compare(other) > 0;
}

bool ResumeToken::operator>=(const ResumeToken& other) const {
    return compare(other) >= 0;
}

Document ResumeToken::toDocument() const {
    return Document{{kDataFieldName, _keyStringData}, {kTypeBitsFieldName, _typeBits}};
}

ResumeToken ResumeToken::parse(const Document& resumeDoc) {
    return ResumeToken(resumeDoc);
}

}  // namespace mongo
