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
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/text.h"

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(DuplicateKeyErrorInfo);

}  // namespace

DuplicateKeyErrorInfo::DuplicateKeyErrorInfo(const BSONObj& keyPattern,
                                             const BSONObj& keyValue,
                                             const BSONObj& collation,
                                             FoundValue&& foundValue)
    : _keyPattern(keyPattern.getOwned()),
      _keyValue(keyValue.getOwned()),
      _collation(collation.getOwned()),
      _foundValue(std::move(foundValue)) {
    if (auto foundValueObj = stdx::get_if<BSONObj>(&_foundValue)) {
        _foundValue = foundValueObj->getOwned();
    }
}

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
            keyValueBuilder.append(keyValueElem.fieldName(),
                                   hexblob::encodeLower(keyValueElem.valueStringData()));
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

    stdx::visit(
        OverloadedVisitor{
            [](stdx::monostate) {},
            [bob](const RecordId& rid) { rid.serializeToken("foundValue", bob); },
            [bob](const BSONObj& obj) {
                if (obj.objsize() < BSONObjMaxUserSize / 2) {
                    bob->append("foundValue", obj);
                }
            },
        },
        _foundValue);
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
                keyValueBuilder.append(keyValueElem.fieldName(),
                                       hexblob::decode(keyValueElem.checkAndGetStringData()));
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

    FoundValue foundValue;
    if (auto foundValueElt = obj["foundValue"]) {
        if (foundValueElt.isABSONObj()) {
            foundValue = foundValueElt.Obj();
        } else {
            foundValue = RecordId::deserializeToken(foundValueElt);
        }
    }

    return std::make_shared<DuplicateKeyErrorInfo>(
        keyPattern, keyValue, collation, std::move(foundValue));
}

}  // namespace mongo
