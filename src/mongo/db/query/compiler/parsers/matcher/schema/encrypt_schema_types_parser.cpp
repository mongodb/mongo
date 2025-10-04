/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/parsers/matcher/schema/encrypt_schema_types_parser.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/query/compiler/parsers/matcher/schema/json_schema_parser.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <vector>

namespace mongo::parsers::matcher::schema {

EncryptSchemaKeyId parseEncryptSchemaKeyId(const BSONElement& element) {
    if (element.type() == BSONType::string) {
        return EncryptSchemaKeyId(element.String());
    } else if (element.type() == BSONType::array) {
        std::vector<UUID> keys;

        for (auto&& arrayElement : element.embeddedObject()) {
            if (arrayElement.type() != BSONType::binData) {
                uasserted(51088,
                          str::stream() << "Encryption schema 'keyId' array elements must "
                                        << "have type BinData, found " << arrayElement.type());
            }
            if (arrayElement.binDataType() == BinDataType::newUUID) {
                const auto uuid = uassertStatusOK(UUID::parse(arrayElement));

                keys.emplace_back(uuid);
            } else {
                uasserted(51084,
                          str::stream()
                              << "Encryption schema 'keyId' array elements must "
                              << "have BinData type UUID, found " << arrayElement.binDataType());
            }
        }
        return EncryptSchemaKeyId(keys);
    } else {
        uasserted(51085,
                  str::stream()
                      << "Expected either string or array of UUID for EncryptSchemaKeyId, found "
                      << element.type());
    }
    MONGO_UNREACHABLE;
}

BSONTypeSet parseBSONTypeSet(const BSONElement& element) {
    // BSON type can be specified with a type alias, other values will be rejected.
    auto typeSet = uassertStatusOK(JSONSchemaParser::parseTypeSet(element, findBSONTypeAlias));
    return BSONTypeSet(typeSet);
}

}  // namespace mongo::parsers::matcher::schema
