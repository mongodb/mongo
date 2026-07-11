// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/keys_collection_util.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace keys_collection_util {

ExternalKeysCollectionDocument makeExternalClusterTimeKeyDoc(BSONObj keyDoc,
                                                             boost::optional<Date_t> expireAt) {
    auto originalKeyDoc = KeysCollectionDocument::parse(keyDoc, IDLParserContext("keyDoc"));

    ExternalKeysCollectionDocument externalKeyDoc(OID::gen(), originalKeyDoc.getKeyId());
    externalKeyDoc.setKeysCollectionDocumentBase(originalKeyDoc.getKeysCollectionDocumentBase());
    externalKeyDoc.setTTLExpiresAt(expireAt);

    return externalKeyDoc;
}

repl::OpTime storeExternalClusterTimeKeyDocs(OperationContext* opCtx,
                                             std::vector<ExternalKeysCollectionDocument> keyDocs) {
    const auto& nss = NamespaceString::kExternalKeysCollectionNamespace;

    for (auto& keyDoc : keyDocs) {
        auto collection = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kWrite),
            MODE_IX);

        writeConflictRetry(opCtx, "storeExternalClusterTimeKeyDocs", nss, [&] {
            const auto filter =
                BSON(ExternalKeysCollectionDocument::kIdFieldName << keyDoc.getId());
            const auto updateMod = keyDoc.toBSON();

            Helpers::upsert(opCtx,
                            collection,
                            filter,
                            updateMod,
                            /*fromMigrate=*/false);
        });
    }

    return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
}

}  // namespace keys_collection_util
}  // namespace mongo
