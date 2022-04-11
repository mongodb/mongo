/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/catalog/collection_options_validation.h"
#include "mongo/crypto/encryption_fields_util.h"

namespace mongo::collection_options_validation {
Status validateStorageEngineOptions(const BSONObj& storageEngine) {
    // Every field inside 'storageEngine' must be a document.
    // Format:
    // {
    //     ...
    //     storageEngine: {
    //         storageEngine1: {
    //             ...
    //         },
    //         storageEngine2: {
    //             ...
    //         }
    //     },
    //     ...
    // }
    for (auto&& elem : storageEngine) {
        if (elem.type() != mongo::Object) {
            return {ErrorCodes::BadValue,
                    str::stream() << "'storageEngine." << elem.fieldName()
                                  << "' must be an embedded document"};
        }
    }
    return Status::OK();
}

EncryptedFieldConfig processAndValidateEncryptedFields(EncryptedFieldConfig config) {

    stdx::unordered_set<UUID, UUID::Hash> keys(config.getFields().size());
    std::vector<FieldRef> fieldPaths;
    fieldPaths.reserve(config.getFields().size());

    for (const auto& field : config.getFields()) {
        UUID keyId = field.getKeyId();

        // Duplicate key ids are bad, it breaks the design
        uassert(6338401, "Duplicate key ids are not allowed", keys.count(keyId) == 0);
        keys.insert(keyId);

        FieldRef newPath(field.getPath());
        for (const auto& path : fieldPaths) {
            uassert(6338402, "Duplicate paths are not allowed", newPath != path);
            // Cannot have indexes on "a" and "a.b"
            uassert(6338403,
                    str::stream() << "Conflicting index paths found as one is a prefix of another '"
                                  << newPath.dottedField() << "' and '" << path.dottedField()
                                  << "'",
                    !path.fullyOverlapsWith(newPath));
        }
        fieldPaths.push_back(std::move(newPath));

        if (field.getQueries().has_value()) {
            auto queriesVariant = field.getQueries().get();

            auto queries = stdx::get_if<std::vector<mongo::QueryTypeConfig>>(&queriesVariant);
            if (queries) {
                // If the user specified multiple queries, verify they are unique
                // TODO - once other index types are added we will need to enhance this check
                uassert(6338404,
                        "Only 1 equality queryType can be specified per field",
                        queries->size() == 1);
            }

            uassert(6412601,
                    "Bson type needs to be specified for equality indexed field",
                    field.getBsonType().has_value());

            BSONType type = typeFromName(field.getBsonType().value());

            uassert(6338405,
                    str::stream() << "Type '" << typeName(type)
                                  << "' is not a supported equality indexed type",
                    isFLE2EqualityIndexedSupportedType(type));
        } else {
            if (field.getBsonType().has_value()) {
                BSONType type = typeFromName(field.getBsonType().value());

                uassert(6338406,
                        str::stream()
                            << "Type '" << typeName(type) << "' is not a supported unindexed type",
                        isFLE2UnindexedSupportedType(type));
            }
        }
    }

    return config;
}

}  // namespace mongo::collection_options_validation
