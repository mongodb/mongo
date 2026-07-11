// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/fts_query_noop.h"

#include <memory>
#include <utility>


namespace mongo {
namespace fts {

std::unique_ptr<FTSQuery> FTSQueryNoop::clone() const {
    auto clonedQuery = std::make_unique<FTSQueryNoop>();
    clonedQuery->setQuery(getQuery());
    clonedQuery->setLanguage(getLanguage());
    clonedQuery->setCaseSensitive(getCaseSensitive());
    clonedQuery->setDiacriticSensitive(getDiacriticSensitive());
    return std::move(clonedQuery);
}

}  // namespace fts
}  // namespace mongo
