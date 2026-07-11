// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/basic_types_gen.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <unicode/coll.h>

namespace mongo {

/**
 * An implementation of the CollatorInterface which is backed by the implementation of collations
 * from the ICU library.
 */
class CollatorInterfaceICU final : public CollatorInterface {
public:
    CollatorInterfaceICU(Collation spec, std::unique_ptr<icu::Collator> collator);

    std::unique_ptr<CollatorInterface> clone() const final;
    std::shared_ptr<CollatorInterface> cloneShared() const final;

    int compare(std::string_view left, std::string_view right) const final;

    ComparisonKey getComparisonKey(std::string_view stringData) const final;

private:
    // The ICU implementation of the collator to which we delegate interesting work. Const methods
    // on the ICU collator are expected to be thread-safe.
    const std::unique_ptr<icu::Collator> _collator;
};

}  // namespace mongo
