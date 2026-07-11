// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_crypto_types.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace [[MONGO_MOD_PRIVATE]] mongo {

class FLEQueryInterfaceMock : public FLEQueryInterface {
public:
    FLEQueryInterfaceMock(OperationContext* opCtx, repl::StorageInterface* storage)
        : _opCtx(opCtx), _storage(storage) {}
    ~FLEQueryInterfaceMock() override = default;

    BSONObj getById(const NamespaceString& nss, BSONElement element) final;

    BSONObj getById(const NamespaceString& nss, PrfBlock block);

    uint64_t countDocuments(const NamespaceString& nss);

    std::vector<std::vector<FLEEdgeCountInfo>> getTags(
        const NamespaceString& nss,
        const std::vector<std::vector<FLEEdgePrfBlock>>& tokensSets,
        FLETagQueryInterface::TagQueryType type) final;

    StatusWith<write_ops::InsertCommandReply> insertDocuments(
        const NamespaceString& nss,
        std::vector<BSONObj> objs,
        StmtId* pStmtId,
        bool translateDuplicateKey,
        bool bypassDocumentValidation = false) final;

    std::pair<write_ops::DeleteCommandReply, BSONObj> deleteWithPreimage(
        const NamespaceString& nss,
        const EncryptionInformation& ei,
        const write_ops::DeleteCommandRequest& deleteRequest) final;

    write_ops::DeleteCommandReply deleteDocument(
        const NamespaceString& nss,
        int32_t stmtId,
        write_ops::DeleteCommandRequest& deleteRequest) final;

    std::pair<write_ops::UpdateCommandReply, BSONObj> updateWithPreimage(
        const NamespaceString& nss,
        const EncryptionInformation& ei,
        const write_ops::UpdateCommandRequest& updateRequest) final;

    write_ops::UpdateCommandReply update(const NamespaceString& nss,
                                         int32_t stmtId,
                                         write_ops::UpdateCommandRequest& updateRequest) final;

    write_ops::FindAndModifyCommandReply findAndModify(
        const NamespaceString& nss,
        const EncryptionInformation& ei,
        const write_ops::FindAndModifyCommandRequest& findAndModifyRequest) final;

    std::vector<BSONObj> findDocuments(const NamespaceString& nss, BSONObj filter) final;

private:
    OperationContext* _opCtx;
    repl::StorageInterface* _storage;
};

}  // namespace mongo
