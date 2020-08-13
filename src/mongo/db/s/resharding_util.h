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
#pragma once

#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/resharded_chunk_gen.h"

namespace mongo {

constexpr auto kReshardingOplogPrePostImageOps = "prePostImageOps"_sd;

/**
 * Asserts that there is not a hole or overlap in the chunks.
 */
void checkForHolesAndOverlapsInChunks(std::vector<ReshardedChunk>& chunks,
                                      const KeyPattern& keyPattern);

/**
 * Validates resharded chunks provided with a reshardCollection cmd. Parses each BSONObj to a valid
 * ReshardedChunk and asserts that each chunk's shardId is associated with an existing entry in
 * the shardRegistry. Then, asserts that there is not a hole or overlap in the chunks.
 */
void validateReshardedChunks(const std::vector<mongo::BSONObj>& chunks,
                             OperationContext* opCtx,
                             const KeyPattern& keyPattern);

/**
 * Asserts that there is not an overlap in the zone ranges.
 */
void checkForOverlappingZones(std::vector<TagsType>& zones);

/**
 * Validates zones provided with a reshardCollection cmd. Parses each BSONObj to a valid
 * TagsType and asserts that each zones's name is associated with an existing entry in
 * config.tags. Then, asserts that there is not an overlap in the zone ranges.
 */
void validateZones(const std::vector<mongo::BSONObj>& zones,
                   const std::vector<TagsType>& authoritativeTags);

/**
 * Create pipeline stages for iterating the buffered copy of the donor oplog and link together the
 * oplog entries with their preImage/postImage oplog. Note that caller is responsible for making
 * sure that the donorOplogNS is properly resolved and ns is set in the expCtx.
 */
std::unique_ptr<Pipeline, PipelineDeleter> createAggForReshardingOplogBuffer(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const BSONObj& resumeToken);

}  // namespace mongo