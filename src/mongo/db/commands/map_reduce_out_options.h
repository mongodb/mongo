/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

enum class OutputType {
    Replace,  // Atomically replace the collection.
    Merge,    // Merge keys, override dups.
    Reduce,   // Merge keys, reduce dups.
    InMemory  // Only store in memory, limited in size.
};

/**
 * Parsed MapReduce Out clause.
 */
class MapReduceOutOptions {
public:
    static MapReduceOutOptions parseFromBSON(const BSONElement& element);

    MapReduceOutOptions() = default;
    MapReduceOutOptions(std::string databaseName,
                        std::string collectionName,
                        const OutputType outputType,
                        bool sharded)
        : _databaseName(std::move(databaseName)),
          _collectionName(std::move(collectionName)),
          _outputType(outputType),
          _sharded(sharded) {}

    void serializeToBSON(StringData fieldName, BSONObjBuilder* builder) const {
        BSONObjBuilder sub(builder->subobjStart(fieldName));
        switch (_outputType) {
            case OutputType::InMemory:
                sub.append("inline", 1);
                break;
            case OutputType::Replace:
                sub.append("replace", _collectionName);
                break;
            case OutputType::Merge:
                sub.append("merge", _collectionName);
                break;
            case OutputType::Reduce:
                sub.append("reduce", _collectionName);
        }
        if (_databaseName != "")
            sub.append("db", _databaseName);
        if (_sharded)
            sub.append("sharded", true);
    }

private:
    std::string _databaseName;
    std::string _collectionName;
    OutputType _outputType;
    bool _sharded;
};

}  // namespace mongo
