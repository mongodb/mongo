// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_crypto_types.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
class FLETagQueryInterface;
}

namespace mongo::fle {

[[MONGO_MOD_PUBLIC]] std::vector<std::vector<FLEEdgeCountInfo>> getCountInfoSets(
    FLETagQueryInterface* queryImpl,
    const NamespaceString& nssEsc,
    ESCDerivedFromDataToken s,
    EDCDerivedFromDataToken d,
    boost::optional<int64_t> cm);

/**
 * Read a list of binary tags given ESC and and EDC derived tokens and a maximum contention
 * factor.
 */
[[MONGO_MOD_PUBLIC]] std::vector<PrfBlock> readTags(FLETagQueryInterface* queryImpl,
                                                    const NamespaceString& nssEsc,
                                                    ESCDerivedFromDataToken s,
                                                    EDCDerivedFromDataToken d,
                                                    boost::optional<int64_t> cm);
}  // namespace mongo::fle
