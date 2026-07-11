// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <span>
#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
                                               std::string_view ident,
                                               std::span<const char> valueBytes);

}  // namespace mongo
