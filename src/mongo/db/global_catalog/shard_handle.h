/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_ref.h"

#include <utility>

namespace mongo {

/**
 * Bundles a shard's ShardId with its unique identifier (shardRef), which may be either a string
 * name or a UUID. When no explicit shardRef is available the name is used as the ref, preserving
 * backward compatibility with older documents that only store the shard name.
 */
class ShardHandle {
public:
    explicit ShardHandle(ShardId name) : _name(std::move(name)), _ref(_name.toString()) {}

    ShardHandle(ShardId name, ShardRef ref) : _name(std::move(name)), _ref(std::move(ref)) {}

    const ShardId& name() const {
        return _name;
    }

    const ShardRef& ref() const {
        return _ref;
    }

private:
    ShardId _name;
    ShardRef _ref;
};

}  // namespace mongo
