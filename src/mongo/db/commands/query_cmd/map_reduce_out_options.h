// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>

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
    MapReduceOutOptions(boost::optional<std::string> databaseName,
                        std::string collectionName,
                        const OutputType outputType,
                        bool sharded)
        : _databaseName(std::move(databaseName)),
          _collectionName(std::move(collectionName)),
          _outputType(outputType),
          _sharded(sharded) {}

    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* builder) const {
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
        if (_databaseName)
            sub.append("db", _databaseName.get());
        if (_sharded)
            sub.append("sharded", true);
    }

    OutputType getOutputType() const {
        return _outputType;
    }

    const std::string& getCollectionName() const {
        return _collectionName;
    }

    const boost::optional<std::string>& getDatabaseName() const {
        return _databaseName;
    }

    bool isSharded() const {
        return _sharded;
    }

private:
    boost::optional<std::string> _databaseName;
    std::string _collectionName;
    OutputType _outputType;
    bool _sharded;
};

}  // namespace mongo
