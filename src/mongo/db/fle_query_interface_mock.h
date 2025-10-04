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

namespace MONGO_MOD_PRIVATE mongo {

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

}  // namespace MONGO_MOD_PRIVATE mongo
