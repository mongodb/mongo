// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/modules.h"

#include <memory>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class BSONObj;
class ServiceContext;

template <typename T>
class StatusWith;

/**
 * An interface which can be used to retrieve a collator.
 */
class CollatorFactoryInterface {
    CollatorFactoryInterface(const CollatorFactoryInterface&) = delete;
    CollatorFactoryInterface& operator=(const CollatorFactoryInterface&) = delete;

public:
    CollatorFactoryInterface() = default;

    virtual ~CollatorFactoryInterface() {}

    /**
     * Returns the CollatorFactoryInterface object associated with the specified service context.
     * This method must only be called if a CollatorFactoryInterface has been set on the service
     * context.
     */
    static CollatorFactoryInterface* get(ServiceContext* serviceContext);

    /**
     * Sets the CollatorFactoryInterface object associated with the specified service context.
     */
    static void set(ServiceContext* serviceContext,
                    std::unique_ptr<CollatorFactoryInterface> collatorFactory);

    /**
     * Parses 'spec' and, on success, returns the corresponding CollatorInterface. If 'spec'
     * represents the simple collation, returns an OK status with a null pointer.
     *
     * Returns a non-OK status if 'spec' is invalid or otherwise cannot be converted into a
     * collator.
     *
     * Returns ErrorCodes::IncompatibleCollationVersion if the collator version does not match the
     * version requested in 'spec'.
     */
    virtual StatusWith<std::unique_ptr<CollatorInterface>> makeFromBSON(const BSONObj& spec) = 0;
};

}  // namespace mongo
