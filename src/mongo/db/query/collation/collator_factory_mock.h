// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/modules.h"

#include <memory>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class CollatorFactoryMock : public CollatorFactoryInterface {
public:
    /**
     * Returns a collator that compares strings by reversing them and performing a binary
     * comparison.
     */
    StatusWith<std::unique_ptr<CollatorInterface>> makeFromBSON(const BSONObj& spec) final;
};

}  // namespace mongo
