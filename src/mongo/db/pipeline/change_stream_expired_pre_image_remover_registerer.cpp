// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/change_stream_expired_pre_image_remover.h"
#include "mongo/db/repl/replica_set_aware_service.h"

namespace mongo {

// A global's constructor runs wherever the object is linked, so linking this registration is what
// subscribes the service to replica set lifecycle events. It lives apart from the service itself so
// that code which only starts or stops the removal job does not also enroll the service in the
// registry of every binary that links it. Binaries that need the service driven by replica set
// lifecycle events depend on this library.
const ReplicaSetAwareServiceRegistry::Registerer<ChangeStreamExpiredPreImagesRemoverService>
    changeStreamExpiredPreImagesRemoverServiceJobRegistryRegisterer(
        "ChangeStreamExpiredPreImagesRemoverService");

}  // namespace mongo
