// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace keys_collection_util {

/*
 * Creates an ExternalKeysCollectionDocument representing an config.external_validation_keys
 * document created based on the given the admin.system.keys document BSONObj.
 */
[[MONGO_MOD_PARENT_PRIVATE]] ExternalKeysCollectionDocument makeExternalClusterTimeKeyDoc(
    BSONObj keyDoc, boost::optional<Date_t> expireAt);

/*
 * Upserts the given ExternalKeysCollectionDocuments into the
 * config.external_validation_keys collection, and returns the optime for the upserts.
 */
[[MONGO_MOD_PARENT_PRIVATE]] repl::OpTime storeExternalClusterTimeKeyDocs(
    OperationContext* opCtx, std::vector<ExternalKeysCollectionDocument> keyDocs);

}  // namespace keys_collection_util
}  // namespace mongo
