// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/duplicate_key_error_info.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/hex.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/text.h"                // IWYU pragma: keep

#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(DuplicateKeyErrorInfo);

}  // namespace

DuplicateKeyErrorInfo::DuplicateKeyErrorInfo(const BSONObj& keyPattern,
                                             const BSONObj& keyValue,
                                             const BSONObj& collation,
                                             FoundValue&& foundValue,
                                             boost::optional<RecordId> duplicateRid)
    : _keyPattern(keyPattern.getOwned()),
      _keyValue(keyValue.getOwned()),
      _collation(collation.getOwned()),
      _foundValue(std::move(foundValue)) {
    if (auto foundValueObj = get_if<BSONObj>(&_foundValue)) {
        _foundValue = foundValueObj->getOwned();
    }
    if (duplicateRid) {
        _duplicateRid = *duplicateRid;
    }
}

void DuplicateKeyErrorInfo::serialize(BSONObjBuilder* bob) const {
    bob->append("keyPattern", _keyPattern);

    // Keep track of which components of the key pattern are hex encoded.
    std::vector<bool> hexEncodedComponents;
    bool atLeastOneComponentIsHexEncoded = false;

    BSONObjBuilder keyValueBuilder{bob->subobjStart("keyValue")};
    for (const auto& keyValueElem : _keyValue) {
        const bool shouldHexEncode = keyValueElem.type() == BSONType::string &&
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

    visit(OverloadedVisitor{
              [](std::monostate) {},
              [bob](const RecordId& rid) { rid.serializeToken("foundValue", bob); },
              [bob](const BSONObj& obj) {
                  if (obj.objsize() < BSONObjMaxUserSize / 2) {
                      bob->append("foundValue", obj);
                  }
              },
          },
          _foundValue);

    if (_duplicateRid) {
        _duplicateRid->serializeToken("duplicateRid", bob);
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

    boost::optional<RecordId> duplicateRid;
    if (auto duplicateRidElt = obj["duplicateRid"]) {
        duplicateRid = RecordId::deserializeToken(duplicateRidElt);
    }

    return std::make_shared<DuplicateKeyErrorInfo>(
        keyPattern, keyValue, collation, std::move(foundValue), duplicateRid);
}

}  // namespace mongo
