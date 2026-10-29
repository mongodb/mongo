// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
