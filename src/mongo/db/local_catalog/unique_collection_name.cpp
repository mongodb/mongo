/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/local_catalog/unique_collection_name.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/query/random_utils.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>

#include <boost/move/utility_core.hpp>

namespace mongo {

StatusWith<NamespaceString> generateRandomCollectionName(OperationContext* opCtx,
                                                         const DatabaseName& dbName,
                                                         StringData collectionNameModel) {
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
        "abcdefghijklmnopqrstuvwxyz"_sd;
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
                                                     StringData collectionNameModel) {
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
