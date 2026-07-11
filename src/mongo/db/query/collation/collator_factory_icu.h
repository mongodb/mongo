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

/**
 * Creates CollatorInterface instances backed by the ICU library's collation implementation.
 *
 * Returns success with a null collator on input {locale: "simple"}.
 *
 * TODO: The factory should open collations once, and then return clones when a caller needs a
 * CollatorInterface. This is more efficient because the necessary read-only data will only be
 * prepared once on collation open.
 */
class CollatorFactoryICU : public CollatorFactoryInterface {
public:
    StatusWith<std::unique_ptr<CollatorInterface>> makeFromBSON(const BSONObj& spec) final;
};

}  // namespace mongo
