/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/type_shard.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

const std::string ShardType::ConfigNS = "config.shards";

const BSONField<std::string> ShardType::name("_id");
const BSONField<std::string> ShardType::host("host");
const BSONField<bool> ShardType::draining("draining");
const BSONField<long long> ShardType::maxSizeMB("maxSize");
const BSONField<BSONArray> ShardType::tags("tags");
const BSONField<ShardType::ShardState> ShardType::state("state");

StatusWith<ShardType> ShardType::fromBSON(const BSONObj& source) {
    ShardType shard;

    {
        std::string shardName;
        Status status = bsonExtractStringField(source, name.name(), &shardName);
        if (!status.isOK())
            return status;
        shard._name = shardName;
    }

    {
        std::string shardHost;
        Status status = bsonExtractStringField(source, host.name(), &shardHost);
        if (!status.isOK())
            return status;
        shard._host = shardHost;
    }

    {
        bool isShardDraining;
        Status status = bsonExtractBooleanField(source, draining.name(), &isShardDraining);
        if (status.isOK()) {
            shard._draining = isShardDraining;
        } else if (status == ErrorCodes::NoSuchKey) {
            // draining field can be mssing in which case it is presumed false
        } else {
            return status;
        }
    }

    {
        long long shardMaxSizeMB;
        // maxSizeMB == 0 means there's no limitation to space usage.
        Status status = bsonExtractIntegerField(source, maxSizeMB.name(), &shardMaxSizeMB);
        if (status.isOK()) {
            shard._maxSizeMB = shardMaxSizeMB;
        } else if (status == ErrorCodes::NoSuchKey) {
            // maxSizeMB field can be missing in which case it is presumed false
        } else {
            return status;
        }
    }

    if (source.hasField(tags.name())) {
        shard._tags = std::vector<std::string>();
        BSONElement tagsElement;
        Status status = bsonExtractTypedField(source, tags.name(), Array, &tagsElement);
        if (!status.isOK())
            return status;

        BSONObjIterator it(tagsElement.Obj());
        while (it.more()) {
            BSONElement tagElement = it.next();
            if (tagElement.type() != String) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "Elements in \"" << tags.name()
                                            << "\" array must be strings but found "
                                            << typeName(tagElement.type()));
            }
            shard._tags->push_back(tagElement.String());
        }
    }

    {
        long long shardState;
        Status status = bsonExtractIntegerField(source, state.name(), &shardState);
        if (status.isOK()) {
            // Make sure the state field falls within the valid range of ShardState values.
            if (!(shardState >= static_cast<std::underlying_type<ShardState>::type>(
                                    ShardState::kNotShardAware) &&
                  shardState <= static_cast<std::underlying_type<ShardState>::type>(
                                    ShardState::kShardAware))) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "Invalid shard state value: " << shardState);
            } else {
                shard._state = static_cast<ShardState>(shardState);
            }
        } else if (status == ErrorCodes::NoSuchKey) {
            // state field can be mssing in which case it is presumed kNotShardAware
        } else {
            return status;
        }
    }

    return shard;
}

Status ShardType::validate() const {
    if (!_name.is_initialized() || _name->empty()) {
        return Status(ErrorCodes::NoSuchKey,
                      str::stream() << "missing " << name.name() << " field");
    }

    if (!_host.is_initialized() || _host->empty()) {
        return Status(ErrorCodes::NoSuchKey,
                      str::stream() << "missing " << host.name() << " field");
    }

    if (_maxSizeMB.is_initialized() && getMaxSizeMB() < 0) {
        return Status(ErrorCodes::BadValue, str::stream() << "maxSize can't be negative");
    }

    return Status::OK();
}

BSONObj ShardType::toBSON() const {
    BSONObjBuilder builder;

    if (_name)
        builder.append(name(), getName());
    if (_host)
        builder.append(host(), getHost());
    if (_draining)
        builder.append(draining(), getDraining());
    if (_maxSizeMB)
        builder.append(maxSizeMB(), getMaxSizeMB());
    if (_tags)
        builder.append(tags(), getTags());
    if (_state)
        builder.append(state(), static_cast<std::underlying_type<ShardState>::type>(getState()));

    return builder.obj();
}

std::string ShardType::toString() const {
    return toBSON().toString();
}

void ShardType::setName(const std::string& name) {
    _name = name;
}

void ShardType::setHost(const std::string& host) {
    invariant(!host.empty());
    _host = host;
}

void ShardType::setDraining(const bool isDraining) {
    _draining = isDraining;
}

void ShardType::setMaxSizeMB(const long long maxSizeMB) {
    invariant(maxSizeMB >= 0);
    _maxSizeMB = maxSizeMB;
}

void ShardType::setTags(const std::vector<std::string>& tags) {
    invariant(tags.size() > 0);
    _tags = tags;
}
void ShardType::setState(const ShardState state) {
    invariant(!_state.is_initialized());
    _state = state;
}

}  // namespace mongo
