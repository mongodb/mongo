/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/db/keys_collection_util.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
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
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
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
