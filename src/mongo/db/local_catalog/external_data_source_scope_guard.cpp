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

#include "mongo/db/local_catalog/external_data_source_scope_guard.h"

#include "mongo/base/status.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/ddl/drop_gen.h"
#include "mongo/db/local_catalog/drop_collection.h"
#include "mongo/db/local_catalog/virtual_collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/external_data_source_option_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

ExternalDataSourceScopeGuard::ExternalDataSourceScopeGuard(
    OperationContext* opCtx,
    const std::vector<std::pair<NamespaceString, std::vector<ExternalDataSourceInfo>>>&
        usedExternalDataSources)
    : _opCtx(opCtx) {
    tassert(8545300,
            "ExternalDataSourceScopeGuard should only be created when there is at least one "
            "external data source.",
            usedExternalDataSources.size() > 0);

    // Just in case that any virtual collection could not be created, when dtor does not have a
    // chance to be executed, cleans up collections that has already been created at that
    // moment.
    ScopeGuard dropVcollGuard([&] { dropVirtualCollections(); });

    for (auto&& [extDataSourceNss, dataSources] : usedExternalDataSources) {
        VirtualCollectionOptions vopts(dataSources);
        uassertStatusOK(createVirtualCollection(opCtx, extDataSourceNss, vopts));
        _toBeDroppedVirtualCollections.emplace_back(extDataSourceNss);
    }

    dropVcollGuard.dismiss();
}

void ExternalDataSourceScopeGuard::dropVirtualCollections() {
    // The move constructor sets '_opCtx' to null when ownership is moved to the other object which
    // means this object must not try to drop collections. There's nothing to drop if '_opCtx' is
    // null.
    if (!_opCtx) {
        return;
    }

    // This function is called in a context of destructor or exception and so guard this against any
    // exceptions.
    try {
        for (auto&& nss : _toBeDroppedVirtualCollections) {
            DropReply reply;
            auto status =
                dropCollection(_opCtx,
                               nss,
                               &reply,
                               DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops);
            if (!status.isOK()) {
                LOGV2_ERROR(6968700, "Failed to drop an external data source", "coll"_attr = nss);
            }
        }
    } catch (...) {
        reportFailedDestructor(MONGO_SOURCE_LOCATION());
    }
}

}  // namespace mongo
