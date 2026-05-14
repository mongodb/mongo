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

#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/modules.h"

namespace mongo::index_builds {

/**
 * Read and parse the document storing the index build resume state from the resume state table,
 * specified by the given ident. Will return boost::none if the record cannot be found or
 * the document fails to parse.
 */
MONGO_MOD_PUBLIC
boost::optional<ResumeIndexInfo> readAndParseResumeIndexInfo(StorageEngine* engine,
                                                             OperationContext* opCtx,
                                                             const std::string& ident);

/**
 * Read the document storing the index build resume state from the resume state table,
 * specified by the given ident, if available, otherwise will return boost::none.
 */
boost::optional<BSONObj> readResumeIndexInfo(StorageEngine* engine,
                                             OperationContext* opCtx,
                                             const std::string& ident);

/**
 * Parse the document storing the index build resume state. Will return boost::none if the document
 * fails to parse.
 */
boost::optional<ResumeIndexInfo> parseResumeIndexInfo(const BSONObj& data);

/**
 * Synthesizes an index build resume state, using default values for the state of each
 * index being built, given the registered metadata about the index build.
 */
ResumeIndexInfo synthesizeResumeIndexInfo(const UUID& buildUUID,
                                          IndexBuildPhaseEnum phase,
                                          const UUID& collectionUUID,
                                          const std::vector<IndexBuildInfo>& indexes);

}  // namespace mongo::index_builds
