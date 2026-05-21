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

#include "mongo/base/string_data.h"
#include "mongo/util/modules.h"

#include <span>

namespace MONGO_MOD_PUBLIC mongo {

class OperationContext;
class ServiceContext;

/**
 * Adds an OpObserver that watches writes to `config.fast_count_metadata_store_timestamps` (the
 * collection-mode store) and to the fast-count timestamps container (via the container-write op
 * observer hooks), and feeds the observed valid-as-of timestamp into the
 * `replicated_fast_count.oplog_lag_secs` gauge. Should be called once per ServiceContext; calling
 * more than once just adds extra (idempotent) observer instances.
 */
void registerReplicatedFastCountOpObserver(ServiceContext* svcCtx);

/**
 * If `ident` matches the fast-count timestamps container, parses the `valid-as-of` field out of
 * `valueBytes` and schedules an on-commit handler on the current recovery unit that calls
 * `recordCheckpointAdvanced`. No-op for any other ident or for an empty/invalid value (e.g.
 * delete ops).
 */
void recordContainerWriteForFastCountTimestamp(OperationContext* opCtx,
                                               StringData ident,
                                               std::span<const char> valueBytes);

}  // namespace MONGO_MOD_PUBLIC mongo
