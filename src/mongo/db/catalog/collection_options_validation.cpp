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

#include "mongo/bson/bsontypes.h"
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
            auto queriesVariant = field.getQueries().value();

            auto queries = stdx::get_if<std::vector<mongo::QueryTypeConfig>>(&queriesVariant);
            mongo::QueryTypeConfig query;

            // TODO SERVER-67421 - remove restriction that only one query type can be specified per
            // field
            if (queries) {
                uassert(
                    6338404, "Only one queryType can be specified per field", queries->size() == 1);
                query = queries->at(0);

            } else {
                query = *stdx::get_if<mongo::QueryTypeConfig>(&queriesVariant);
            }

            uassert(6412601,
                    "Bson type needs to be specified for an indexed field",
                    field.getBsonType().has_value());

            switch (query.getQueryType()) {
                case QueryTypeEnum::Equality: {
                    BSONType type = typeFromName(field.getBsonType().value());

                    uassert(6338405,
                            str::stream() << "Type '" << typeName(type)
                                          << "' is not a supported equality indexed type",
                            isFLE2EqualityIndexedSupportedType(type));

                    uassert(6775205,
                            "The field 'sparsity' is not allowed for equality index but is present",
                            !query.getSparsity().has_value());
                    uassert(6775206,
                            "The field 'min' is not allowed for equality index but is present",
                            !query.getMin().has_value());
                    uassert(6775207,
                            "The field 'max' is not allowed for equality index but is present",
                            !query.getMax().has_value());
                    break;
                }
                case QueryTypeEnum::Range: {
                    BSONType type = typeFromName(field.getBsonType().value());

                    uassert(6775201,
                            str::stream() << "Type '" << typeName(type)
                                          << "' is not a supported range indexed type",
                            isFLE2RangeIndexedSupportedType(type));

                    uassert(6775202,
                            "The field 'sparsity' is missing but required for range index",
                            query.getSparsity().has_value());
                    uassert(6775203,
                            "The field 'min' is missing but required for range index",
                            query.getMin().has_value());
                    uassert(6775204,
                            "The field 'max' is missing but required for range index",
                            query.getMax().has_value());

                    uassert(6775214,
                            "The field 'sparsity' must be between 1 and 4",
                            query.getSparsity().value() >= 1 && query.getSparsity().value() <= 4);

                    // Check type compatibility
                    switch (type) {
                        case NumberInt: {
                            int min = query.getMin()->coerceToInt();
                            int max = query.getMax()->coerceToInt();

                            uassert(6775208, "Min must be less than max", min < max);
                            break;
                        }
                        case NumberLong: {
                            long min = query.getMin()->coerceToLong();
                            long max = query.getMax()->coerceToLong();
                            uassert(6775209, "Min must be less than max", min < max);
                            break;
                        }
                        case NumberDouble: {
                            double min = query.getMin()->coerceToDouble();
                            double max = query.getMax()->coerceToDouble();
                            uassert(6775210, "Min must be less than max", min < max);
                            break;
                        }
                        case NumberDecimal: {
                            Decimal128 min = query.getMin()->coerceToDecimal();
                            Decimal128 max = query.getMax()->coerceToDecimal();
                            uassert(6775211, "Min must be less than max", min < max);
                            break;
                        }
                        case Date: {
                            uassert(6775212,
                                    "Min and max must be a date type when bsonType is date",
                                    query.getMin()->getType() == Date &&
                                        query.getMax()->getType() == Date);

                            Date_t min = query.getMin()->getDate();
                            Date_t max = query.getMax()->getDate();
                            uassert(6775213, "Min must be less than max", min < max);
                            break;
                        }
                        default:
                            MONGO_COMPILER_UNREACHABLE;
                    }

                    break;
                }
                default:
                    MONGO_COMPILER_UNREACHABLE;
            }
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
