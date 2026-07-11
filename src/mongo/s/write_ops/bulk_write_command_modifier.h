// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/commands/query_cmd/bulk_write_crud_op.h"
#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <tuple>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Helper functions which add new operations into an existing BulkWriteCommandRequest.
 * Only used from tests.
 */
class [[MONGO_MOD_FILE_PRIVATE]] BulkWriteCommandModifier {
public:
    BulkWriteCommandModifier(BulkWriteCommandRequest* request, size_t capacity = 0)
        : _request(request), _ops(request->getOps()), _nsInfos(request->getNsInfo()) {
        invariant(_request);
        for (size_t i = 0; i < _nsInfos.size(); i++) {
            auto nsInfo = _nsInfos[i];
            _nsInfoIdxes[nsInfo.getNs()] = i;
        }

        if (capacity > 0) {
            _ops.reserve(capacity);
        }
    }

    BulkWriteCommandModifier(BulkWriteCommandModifier&&) = default;

    /**
     * This function must be called for the BulkWriteCommandRequest to be in a usable state.
     */
    void finishBuild();

    void addOp(const write_ops::InsertCommandRequest& insertOp);
    void addOp(const write_ops::UpdateCommandRequest& updateOp);
    void addOp(const write_ops::DeleteCommandRequest& deleteOp);

    void addInsert(const OpMsgRequest& request);
    void addUpdate(const OpMsgRequest& request);
    void addDelete(const OpMsgRequest& request);

    size_t numOps() const {
        return _request->getOps().size();
    }

    void setIsTimeseriesNamespace(const NamespaceString& nss, bool isTimeseriesNamespace) {
        auto [nsInfoEntry, idx] = getNsInfoEntry(nss);
        nsInfoEntry.setIsTimeseriesNamespace(isTimeseriesNamespace);
    }

    void setEncryptionInformation(const NamespaceString& nss,
                                  const EncryptionInformation& encryption) {
        auto [nsInfoEntry, idx] = getNsInfoEntry(nss);
        nsInfoEntry.setEncryptionInformation(encryption);
    }

    void setShardVersion(const NamespaceString& nss, const ShardVersion& sv) {
        auto [nsInfoEntry, idx] = getNsInfoEntry(nss);
        nsInfoEntry.setShardVersion(sv);
    }

    const ShardVersion& getShardVersion(const NamespaceString& nss) {
        auto [nsInfoEntry, idx] = getNsInfoEntry(nss);
        invariant(nsInfoEntry.getShardVersion());
        return *nsInfoEntry.getShardVersion();
    }

    void setDbVersion(const NamespaceString& nss, const DatabaseVersion& dbv) {
        auto [nsInfoEntry, idx] = getNsInfoEntry(nss);
        nsInfoEntry.setDatabaseVersion(dbv);
    }

    const DatabaseVersion& getDbVersion(const NamespaceString& nss) {
        auto [nsInfoEntry, idx] = getNsInfoEntry(nss);
        invariant(nsInfoEntry.getDatabaseVersion());
        return *nsInfoEntry.getDatabaseVersion();
    }

    void addInsertOps(const NamespaceString& nss, std::vector<BSONObj> docs);

    void addUpdateOp(const NamespaceString& nss,
                     const BSONObj& query,
                     const BSONObj& update,
                     bool upsert,
                     bool multi,
                     const boost::optional<std::vector<BSONObj>>& arrayFilters,
                     const boost::optional<BSONObj>& sort,
                     const boost::optional<BSONObj>& collation,
                     const boost::optional<BSONObj>& hint);

    void addPipelineUpdateOps(const NamespaceString& nss,
                              const BSONObj& query,
                              const std::vector<BSONObj>& updates,
                              bool upsert,
                              bool useMultiUpdate);

    void addDeleteOp(const NamespaceString& nss,
                     const BSONObj& query,
                     bool multiDelete,
                     const boost::optional<BSONObj>& collation,
                     const boost::optional<BSONObj>& hint);

private:
    BulkWriteCommandRequest* _request;

    stdx::unordered_map<NamespaceString, size_t> _nsInfoIdxes;

    std::vector<BulkWriteOpVariant> _ops;
    std::vector<mongo::NamespaceInfoEntry> _nsInfos;

    /**
     * Gets the NamespaceInfoEntry for the associated namespace. If one does not exist
     * then it will be created. Returns a reference to the NamespaceInfoEntry and the index in
     * the nsInfo array.
     */
    std::tuple<NamespaceInfoEntry&, size_t> getNsInfoEntry(const NamespaceString& nss);

    void parseRequestFromOpMsg(const NamespaceString& nss, const OpMsgRequest& request);
};

}  // namespace mongo
