// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/unique_collection_name.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/query/random_utils.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

StatusWith<NamespaceString> generateRandomCollectionName(OperationContext* opCtx,
                                                         const DatabaseName& dbName,
                                                         std::string_view collectionNameModel) {
    // There must be at least one percent sign in the collection name model.
    auto numPercentSign = std::count(collectionNameModel.begin(), collectionNameModel.end(), '%');
    if (numPercentSign == 0) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream()
                          << "Cannot generate collection name for temporary collection: "
                             "model for collection name "
                          << collectionNameModel << " must contain at least one percent sign.");
    }

    static constexpr auto charsToChooseFrom =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"sv;
    static constexpr auto charsetSize = charsToChooseFrom.size();

    static_assert((10U + 26U * 2) == charsetSize);

    auto& rng = random_utils::getRNG();

    auto replacePercentSign = [&](char c) {
        if (c != '%') {
            return c;
        }
        auto i = rng.nextInt32(charsetSize);
        return charsToChooseFrom[i];
    };

    auto collectionName = std::string{collectionNameModel};
    std::transform(
        collectionName.begin(), collectionName.end(), collectionName.begin(), replacePercentSign);

    return NamespaceStringUtil::deserialize(dbName, collectionName);
}

StatusWith<NamespaceString> makeUniqueCollectionName(OperationContext* opCtx,
                                                     const DatabaseName& dbName,
                                                     std::string_view collectionNameModel) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(dbName, MODE_IX));

    static constexpr auto kNumGenerationAttempts = 30'000;
    for (auto i = 0; i < kNumGenerationAttempts; ++i) {
        auto nss = generateRandomCollectionName(opCtx, dbName, collectionNameModel);

        if (!nss.isOK()) {
            return nss;
        }

        if (!CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss.getValue())) {
            return nss;
        }
    }

    return Status(
        ErrorCodes::NamespaceExists,
        str::stream() << "Cannot generate collection name for temporary collection with model "
                      << collectionNameModel << " after " << kNumGenerationAttempts
                      << " attempts due to namespace conflicts with existing collections.");
}

}  // namespace mongo
