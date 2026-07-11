// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/search/search_index_process_interface.h"

#include "mongo/db/service_context.h"

namespace mongo {

Service::Decoration<std::unique_ptr<SearchIndexProcessInterface>>
    searchIndexProcessInterfaceDecoration =
        Service::declareDecoration<std::unique_ptr<SearchIndexProcessInterface>>();

SearchIndexProcessInterface* SearchIndexProcessInterface::get(Service* service) {
    invariant(searchIndexProcessInterfaceDecoration(service).get());
    return searchIndexProcessInterfaceDecoration(service).get();
}

SearchIndexProcessInterface* SearchIndexProcessInterface::get(OperationContext* ctx) {
    return get(ctx->getService());
}

void SearchIndexProcessInterface::set(Service* service,
                                      std::unique_ptr<SearchIndexProcessInterface> impl) {
    invariant(!searchIndexProcessInterfaceDecoration(service).get());
    searchIndexProcessInterfaceDecoration(service) = std::move(impl);
}

}  // namespace mongo
