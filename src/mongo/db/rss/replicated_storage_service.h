// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/rss/persistence_provider.h"
#include "mongo/db/rss/service_lifecycle.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

namespace mongo::rss {

class [[MONGO_MOD_PUBLIC]] ReplicatedStorageService {
public:
    static ReplicatedStorageService& get(ServiceContext*);
    static ReplicatedStorageService& get(OperationContext*);

    PersistenceProvider& getPersistenceProvider();
    const PersistenceProvider& getPersistenceProvider() const;
    void setPersistenceProvider(std::unique_ptr<PersistenceProvider>&&);

    PersistenceProvider& getSpillPersistenceProvider();
    void setSpillPersistenceProvider(std::unique_ptr<PersistenceProvider>&&);

    ServiceLifecycle& getServiceLifecycle();
    void setServiceLifecycle(std::unique_ptr<ServiceLifecycle>&&);

private:
    std::unique_ptr<PersistenceProvider> _provider;
    std::unique_ptr<PersistenceProvider> _spillProvider;
    std::unique_ptr<ServiceLifecycle> _lifecycle;
};

}  // namespace mongo::rss
