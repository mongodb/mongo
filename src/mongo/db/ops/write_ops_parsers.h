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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {
namespace write_ops {

// Conservative per array element overhead. This value was calculated as 1 byte (element type) + 5
// bytes (max string encoding of the array index encoded as string and the maximum key is 99999) + 1
// byte (zero terminator) = 7 bytes
constexpr int kWriteCommandBSONArrayPerElementOverheadBytes = 7;

/**
 * Parses the 'limit' property of a delete entry, which has inverted meaning from the 'multi'
 * property of an update.
 */
bool readMultiDeleteProperty(const BSONElement& limitElement);

/**
 * Writes the 'isMulti' value as a limit property.
 */
void writeMultiDeleteProperty(bool isMulti, StringData fieldName, BSONObjBuilder* builder);

class UpdateModification {
public:
    enum class Type { kClassic, kPipeline };

    static StringData typeToString(Type type) {
        return (type == Type::kClassic ? "Classic"_sd : "Pipeline"_sd);
    }

    UpdateModification() = default;
    UpdateModification(BSONElement update);

    // This constructor exists only to provide a fast-path for constructing classic-style updates.
    UpdateModification(const BSONObj& update);


    /**
     * These methods support IDL parsing of the "u" field from the update command and OP_UPDATE.
     */
    static UpdateModification parseFromBSON(BSONElement elem);
    void serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const;

    // When parsing from legacy OP_UPDATE messages, we receive the "u" field as an object. When an
    // array is parsed, we receive it as an object with numeric fields names and can't differentiate
    // between a user constructed object and an array. For that reason, we don't support pipeline
    // style update via OP_UPDATE and 'obj' is assumed to be a classic update.
    //
    // If a user did send a pipeline-style update via OP_UPDATE, it would fail parsing a field
    // representing an aggregation stage, due to the leading '$'' character.
    static UpdateModification parseLegacyOpUpdateFromBSON(const BSONObj& obj);

    int objsize() const {
        if (_type == Type::kClassic) {
            return _classicUpdate->objsize();
        }

        int size = 0;
        std::for_each(_pipeline->begin(), _pipeline->end(), [&size](const BSONObj& obj) {
            size += obj.objsize() + kWriteCommandBSONArrayPerElementOverheadBytes;
        });

        return size + kWriteCommandBSONArrayPerElementOverheadBytes;
    }

    Type type() const {
        return _type;
    }

    BSONObj getUpdateClassic() const {
        invariant(_type == Type::kClassic);
        return *_classicUpdate;
    }

    const std::vector<BSONObj>& getUpdatePipeline() const {
        invariant(_type == Type::kPipeline);
        return *_pipeline;
    }

    std::string toString() const {
        StringBuilder sb;
        sb << "{type: " << typeToString(_type) << ", update: ";

        if (_type == Type::kClassic) {
            sb << *_classicUpdate << "}";
        } else {
            sb << Value(*_pipeline).toString();
        }

        return sb.str();
    }

private:
    Type _type = Type::kClassic;
    boost::optional<BSONObj> _classicUpdate;
    boost::optional<std::vector<BSONObj>> _pipeline;
};

}  // namespace write_ops
}  // namespace mongo
