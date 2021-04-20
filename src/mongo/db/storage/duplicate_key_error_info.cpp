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

#include "mongo/db/storage/duplicate_key_error_info.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/text.h"

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(DuplicateKeyErrorInfo);

}  // namespace

void DuplicateKeyErrorInfo::serialize(BSONObjBuilder* bob) const {
    bob->append("keyPattern", _keyPattern);

    // Keep track of which components of the key pattern are hex encoded.
    std::vector<bool> hexEncodedComponents;
    bool atLeastOneComponentIsHexEncoded = false;

    BSONObjBuilder keyValueBuilder{bob->subobjStart("keyValue")};
    for (const auto& keyValueElem : _keyValue) {
        const bool shouldHexEncode = keyValueElem.type() == BSONType::String &&
            (!_collation.isEmpty() || !isValidUTF8(keyValueElem.valueStringData()));

        hexEncodedComponents.push_back(shouldHexEncode);
        if (shouldHexEncode) {
            atLeastOneComponentIsHexEncoded = true;
            auto elem = keyValueElem.valueStringData();
            keyValueBuilder.append(keyValueElem.fieldName(),
                                   toHexLower(elem.rawData(), elem.size()));
        } else {
            keyValueBuilder.append(keyValueElem);
        }
    }
    keyValueBuilder.doneFast();

    // Append a vector of booleans describing which components of the key pattern are hex encoded.
    if (atLeastOneComponentIsHexEncoded) {
        BSONArrayBuilder hexEncodedBuilder{bob->subarrayStart("hexEncoded")};
        for (auto&& isHex : hexEncodedComponents) {
            hexEncodedBuilder.appendBool(isHex);
        }
        hexEncodedBuilder.doneFast();
    }

    if (!_collation.isEmpty()) {
        bob->append("collation", _collation);
    }
}

std::shared_ptr<const ErrorExtraInfo> DuplicateKeyErrorInfo::parse(const BSONObj& obj) {
    auto keyPattern = obj["keyPattern"].Obj();
    BSONObj keyValue;

    // Determine which components of 'keyValue' are hex encoded that need to be decoded.
    // If the "hexEncoded" field does not exist, then assume that no decoding is necessary.
    if (auto hexEncodedElt = obj["hexEncoded"]) {
        BSONObjIterator isHexEncodedIt(hexEncodedElt.Obj());
        BSONObjIterator keyValueElemIt(obj["keyValue"].Obj());
        BSONObjBuilder keyValueBuilder;
        while (isHexEncodedIt.more()) {
            const auto& keyValueElem = keyValueElemIt.next();

            if (isHexEncodedIt.next().Bool()) {
                StringBuilder out;
                const StringData value = keyValueElem.checkAndGetStringData();
                for (size_t i = 0; i < value.size(); i += 2) {
                    out << uassertStatusOK(fromHex(StringData(&value.rawData()[i], 2)));
                }
                keyValueBuilder.append(keyValueElem.fieldName(), out.str());
            } else {
                keyValueBuilder.append(keyValueElem);
            }
        }
        keyValue = keyValueBuilder.obj();
    } else {
        keyValue = obj["keyValue"].Obj();
    }

    BSONObj collation;
    if (auto collationElt = obj["collation"]) {
        collation = collationElt.Obj();
    }

    return std::make_shared<DuplicateKeyErrorInfo>(keyPattern, keyValue, collation);
}

}  // namespace mongo
