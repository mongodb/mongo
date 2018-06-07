/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <string>

#include "mongo/base/string_data.h"

namespace mongo {
class AddShard;
class BSONObj;
class OperationContext;
class ShardId;

// Contains a collection of utility functions relating to the addShard command
namespace add_shard_util {

/*
 * The _id value for shard identity documents
 */
constexpr StringData kShardIdentityDocumentId = "shardIdentity"_sd;

/**
 * Creates an AddShard command object that's sent from the config server to
 * a mongod to instruct it to initialize itself as a shard in the cluster.
 */
AddShard createAddShardCmd(OperationContext* opCtx, const ShardId& shardName);

/**
 * Returns a BSON representation of an update request that can be used to insert a shardIdentity
 * doc into the shard with the given shardName (or update the shard's existing shardIdentity
 * doc's configsvrConnString if the _id, shardName, and clusterId do not conflict).
 */
BSONObj createShardIdentityUpsertForAddShard(const AddShard& addShardCmd);

}  // namespace mongo
}  // namespace add_shard_util
