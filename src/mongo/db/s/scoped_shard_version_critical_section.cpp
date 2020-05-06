/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/scoped_shard_version_critical_section.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"

namespace mongo {

ScopedShardVersionCriticalSection::ScopedShardVersionCriticalSection(OperationContext* opCtx,
                                                                     NamespaceString nss)
    : _nss(std::move(nss)), _opCtx(opCtx) {

    {
        // Enters the commit phase of the critical section and blocks reads
        AutoGetCollection autoColl(_opCtx,
                                   _nss,
                                   MODE_X,
                                   AutoGetCollection::ViewMode::kViewsForbidden,
                                   _opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                       Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));
        auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
        csr->enterCriticalSectionCatchUpPhase(_opCtx);
    }

    forceShardFilteringMetadataRefresh(_opCtx, _nss, true);
}

ScopedShardVersionCriticalSection::~ScopedShardVersionCriticalSection() {
    UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
    AutoGetCollection autoColl(_opCtx, _nss, MODE_IX);
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    csr->exitCriticalSection(_opCtx);
}

void ScopedShardVersionCriticalSection::enterCommitPhase() {
    AutoGetCollection autoColl(_opCtx,
                               _nss,
                               MODE_X,
                               AutoGetCollection::ViewMode::kViewsForbidden,
                               _opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                   Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    csr->enterCriticalSectionCommitPhase(_opCtx);
}

}  // namespace mongo
