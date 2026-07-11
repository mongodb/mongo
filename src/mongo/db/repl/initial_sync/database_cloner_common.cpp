// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/initial_sync/database_cloner_common.h"

#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {

CollectionOptions parseCollectionOptionsForDatabaseCloner(const BSONObj& obj) {
    return uassertStatusOK(CollectionOptions::parse(obj, CollectionOptions::parseForStorage));
}

}  // namespace repl
}  // namespace mongo
