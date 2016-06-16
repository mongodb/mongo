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

#pragma once

#include <boost/optional.hpp>
#include <string>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/write_ops/batched_update_request.h"

namespace mongo {

struct BSONArray;
class BSONObj;
class Status;
template <typename T>
class StatusWith;

/**
 * This class represents the layout and contents of documents contained in the
 * config.shards collection. All manipulation of documents coming from that
 * collection should be done with this class.
 */
class ShardType {
public:
    enum class ShardState : int {
        kNotShardAware = 0,
        kShardAware,
    };

    // Name of the shards collection in the config server.
    static const std::string ConfigNS;

    // Field names and types in the shards collection type.
    static const BSONField<std::string> name;
    static const BSONField<std::string> host;
    static const BSONField<bool> draining;
    static const BSONField<long long> maxSizeMB;
    static const BSONField<BSONArray> tags;
    static const BSONField<ShardState> state;


    /**
     * Constructs a new ShardType object from BSON.
     * Also does validation of the contents.
     */
    static StatusWith<ShardType> fromBSON(const BSONObj& source);

    /**
     * Returns OK if all fields have been set. Otherwise returns NoSuchKey
     * and information about the first field that is missing.
     */
    Status validate() const;

    /**
     * Returns the BSON representation of the entry.
     */
    BSONObj toBSON() const;

    /**
     * Returns a std::string representation of the current internal state.
     */
    std::string toString() const;

    const std::string& getName() const {
        return _name.get();
    }
    void setName(const std::string& name);

    const std::string& getHost() const {
        return _host.get();
    }
    void setHost(const std::string& host);

    bool getDraining() const {
        return _draining.value_or(false);
    }
    void setDraining(const bool draining);

    long long getMaxSizeMB() const {
        return _maxSizeMB.value_or(0);
    }
    void setMaxSizeMB(const long long maxSizeMB);

    const std::vector<std::string> getTags() const {
        return _tags.value_or(std::vector<std::string>());
    }
    void setTags(const std::vector<std::string>& tags);

    const ShardState getState() const {
        return _state.value_or(ShardState::kNotShardAware);
    }
    void setState(const ShardState state);

private:
    // Convention: (M)andatory, (O)ptional, (S)pecial rule.

    // (M)  shard's id
    boost::optional<std::string> _name;
    // (M)  connection string for the host(s)
    boost::optional<std::string> _host;
    // (O) is it draining chunks?
    boost::optional<bool> _draining;
    // (O) maximum allowed disk space in MB
    boost::optional<long long> _maxSizeMB;
    // (O) shard tags
    boost::optional<std::vector<std::string>> _tags;
    // (O) shard state
    boost::optional<ShardState> _state;
};

}  // namespace mongo
