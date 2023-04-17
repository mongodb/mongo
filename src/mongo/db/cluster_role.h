/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

namespace mongo {
/**
 * ClusterRole is not mutually exclusive when featureFlagCatalogShard is true. In this mode, a
 * config server cluster role is also a shard server cluster role.
 */
class ClusterRole {
public:
    enum Value {
        None,
        ShardServer,
        ConfigServer,
    };

    ClusterRole(Value v = ClusterRole::None) : _value(v) {}

    ClusterRole& operator=(const ClusterRole& rhs) {
        if (this != &rhs) {
            _value = rhs._value;
        }
        return *this;
    }

    bool has(const ClusterRole& other) const;

    // Returns true if this mongod was started with --shardsvr, false otherwise.
    bool exclusivelyHasShardRole();

    // Returns true if this mongod was started with --configsvr in a non-config shard topology,
    // false otherwise.
    // TODO SERVER-75391: Remove.
    bool exclusivelyHasConfigRole();

private:
    Value _value;
};
}  // namespace mongo
