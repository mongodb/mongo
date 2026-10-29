// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/collation/collator_factory_mock.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_interface_mock.h"

#include <memory>
#include <utility>


namespace mongo {

StatusWith<std::unique_ptr<CollatorInterface>> CollatorFactoryMock::makeFromBSON(
    const BSONObj& spec) {
    if (SimpleBSONObjComparator::kInstance.evaluate(spec == CollationSpec::kSimpleSpec)) {
        return {nullptr};
    }
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    return {std::move(collator)};
}

}  // namespace mongo
