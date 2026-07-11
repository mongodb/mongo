// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo::catalog_helper_ddl {
/*
 * The following class is used to acquire collections and views for catalog DDLS.
 * DDLs usually require to either access a list of collections or a list of collections and views.
 * When one of the nss is a view, we need to acquire the system.views collection as well.
 * It is important to lock the system.views together with the target view.
 * Once system.views is acquired, it will access the same snapshot as the target view, which could
 represent a stale version of the system collection if left unlocked.
 * DDLs assume to have access to the latest version of any resource they access.

 * This is just a trivial wrapper on top of CollectionOrViewAcquisitionMap to easily retrieve the
 * system.views collection.
 */

// TODO (SERVER-120463) remove [[MONGO_MOD_UNFORTUNATELY_OPEN]]
class [[MONGO_MOD_UNFORTUNATELY_OPEN]] AcquisitionsForCatalogWrites {
public:
    AcquisitionsForCatalogWrites(const CollectionOrViewAcquisitionMap& acquisitions)
        : _acquisitions(acquisitions) {
        for (auto& acq : _acquisitions) {
            if (acq.first.isSystemDotViews()) {
                _systemViewsAcquisition = acq.second.getCollection();
            }
        }
    }

    const CollectionOrViewAcquisition& at(const NamespaceString& nss) const {
        return _acquisitions.at(nss);
    }

    const boost::optional<CollectionAcquisition>& getSystemViews() const {
        return _systemViewsAcquisition;
    }


    bool contains(const NamespaceString& nss) const {
        return _acquisitions.contains(nss);
    }

private:
    CollectionOrViewAcquisitionMap _acquisitions;
    boost::optional<CollectionAcquisition> _systemViewsAcquisition;
};

// TODO (SERVER-120463) remove [[MONGO_MOD_UNFORTUNATELY_OPEN]]
[[MONGO_MOD_UNFORTUNATELY_OPEN]] AcquisitionsForCatalogWrites
acquireCollectionOrViewForCatalogWrites(OperationContext* opCtx,
                                        const CollectionOrViewAcquisitionRequests& requests);

}  // namespace mongo::catalog_helper_ddl
