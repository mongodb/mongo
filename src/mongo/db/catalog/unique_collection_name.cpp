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

#include "mongo/db/catalog/unique_collection_name.h"

#include "mongo/db/catalog/collection_catalog.h"

namespace mongo {
namespace {

Mutex uniqueCollectionNameMutex = MONGO_MAKE_LATCH("UniqueCollectionNameMutex");

// Random number generator used to create unique collection namespaces suitable for temporary
// collections
PseudoRandom uniqueCollectionNamespacePseudoRandom(Date_t::now().asInt64());

}  // namespace

StatusWith<NamespaceString> makeUniqueCollectionName(OperationContext* opCtx,
                                                     const DatabaseName& dbName,
                                                     StringData collectionNameModel) {
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_IX));

    // There must be at least one percent sign in the collection name model.
    auto numPercentSign = std::count(collectionNameModel.begin(), collectionNameModel.end(), '%');
    if (numPercentSign == 0) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream()
                          << "Cannot generate collection name for temporary collection: "
                             "model for collection name "
                          << collectionNameModel << " must contain at least one percent sign.");
    }

    const auto charsToChooseFrom =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"_sd;
    invariant((10U + 26U * 2) == charsToChooseFrom.size());

    stdx::lock_guard<Latch> lk(uniqueCollectionNameMutex);

    auto replacePercentSign = [&](char c) {
        if (c != '%') {
            return c;
        }
        auto i = uniqueCollectionNamespacePseudoRandom.nextInt32(charsToChooseFrom.size());
        return charsToChooseFrom[i];
    };

    auto numGenerationAttempts = numPercentSign * charsToChooseFrom.size() * 100U;
    for (decltype(numGenerationAttempts) i = 0; i < numGenerationAttempts; ++i) {
        auto collectionName = collectionNameModel.toString();
        std::transform(collectionName.begin(),
                       collectionName.end(),
                       collectionName.begin(),
                       replacePercentSign);

        NamespaceString nss(dbName, collectionName);
        if (!CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss)) {
            return nss;
        }
    }

    return Status(
        ErrorCodes::NamespaceExists,
        str::stream() << "Cannot generate collection name for temporary collection with model "
                      << collectionNameModel << " after " << numGenerationAttempts
                      << " attempts due to namespace conflicts with existing collections.");
}

}  // namespace mongo
