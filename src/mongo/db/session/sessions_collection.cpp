// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/session/sessions_collection.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/session/logical_session_cache_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/sessions_server_parameters_gen.h"
#include "mongo/db/shard_role/ddl/create_indexes_gen.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result_write_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {


BSONObj lsidQuery(const LogicalSessionId& lsid) {
    return BSON(LogicalSessionRecord::kIdFieldName << lsid.toBSON());
}

BSONObj lsidQuery(const LogicalSessionRecord& record) {
    return lsidQuery(record.getId());
}

BSONArray updateQuery(const LogicalSessionRecord& record) {
    // [ { $set : { lastUse : $$NOW } } , { $set : { user: <user> } } ]

    // Build our update doc.
    BSONArrayBuilder updateBuilder;
    updateBuilder << BSON("$set" << BSON(LogicalSessionRecord::kLastUseFieldName << "$$NOW"));

    if (record.getUser()) {
        updateBuilder << BSON("$set" << BSON(LogicalSessionRecord::kUserFieldName
                                             << BSON("name" << *record.getUser())));
    }

    return updateBuilder.arr();
}
struct BulkResult {
    std::vector<size_t> failedIndices;
    std::vector<Status> errors;  // One per batch that had issues

    bool hasErrors() const {
        return !errors.empty();
    }
};

template <typename TFactory, typename AddLineFn, typename SendFn, typename Container>
BulkResult runBulkGeneric(TFactory makeT,
                          AddLineFn addLine,
                          SendFn sendBatch,
                          const Container& items) {
    using T = decltype(makeT());

    size_t i = 0;
    size_t batchStartIdx = 0;
    boost::optional<T> thing;
    std::vector<size_t> idxs;
    std::vector<Status> failStatuses;

    auto setupBatch = [&] {
        batchStartIdx += i;
        i = 0;
        thing.emplace(makeT());
    };

    auto sendLocalBatch = [&] {
        Status batchResult = sendBatch(thing.value());
        if (!batchResult.isOK()) {
            failStatuses.push_back(batchResult);
            for (size_t idx = 0; idx < i; idx++) {
                idxs.push_back(batchStartIdx + idx);
            }
        }
    };

    setupBatch();

    for (const auto& item : items) {
        addLine(*thing, item);

        if (++i >= std::size_t(mongo::gSessionMaxBatchSize.load())) {
            sendLocalBatch();
            setupBatch();
        }
    }

    if (i > 0) {
        sendLocalBatch();
    }
    return {idxs, failStatuses};
}

template <typename InitBatchFn, typename AddLineFn, typename SendBatchFn, typename Container>
BulkResult runBulkCmd(std::string_view label,
                      InitBatchFn&& initBatch,
                      AddLineFn&& addLine,
                      SendBatchFn&& sendBatch,
                      const Container& items) {
    BufBuilder buf;

    boost::optional<BSONObjBuilder> batchBuilder;
    boost::optional<BSONArrayBuilder> entries;

    auto makeBatch = [&] {
        buf.reset();
        batchBuilder.emplace(buf);
        initBatch(&(batchBuilder.value()));
        entries.emplace(batchBuilder->subarrayStart(label));

        return &(entries.value());
    };

    auto sendLocalBatch = [&](BSONArrayBuilder*) {
        entries->done();
        return sendBatch(batchBuilder->done());
    };

    return runBulkGeneric(makeBatch, addLine, sendLocalBatch, items);
}

}  // namespace

constexpr std::string_view SessionsCollection::kSessionsTTLIndex;

SessionsCollection::SessionsCollection() = default;

SessionsCollection::~SessionsCollection() = default;

std::function<Status(BSONObj)> SessionsCollection::withRefreshTimeout(
    std::function<Status(BSONObj)> fn) {
    if (!logicalSessionCacheJobTimeoutEnabled) {
        return fn;
    }
    const auto timeoutMs = static_cast<long long>(logicalSessionRefreshMillis) * 9 / 10;
    return [fn = std::move(fn), timeoutMs](BSONObj batch) {
        BSONObjBuilder builder(batch);
        builder.append("maxTimeMS", timeoutMs);
        return fn(builder.obj());
    };
}

SessionsCollection::SendBatchFn SessionsCollection::makeSendFnForBatchWrite(
    const NamespaceString& ns, DBClientBase* client) {
    auto send = [client, ns](BSONObj batch) {
        BSONObj res;
        client->runCommand(ns.dbName(), batch, res);
        return getStatusFromWriteCommandReply(res);
    };

    return send;
}

SessionsCollection::SendBatchFn SessionsCollection::makeSendFnForCommand(const NamespaceString& ns,
                                                                         DBClientBase* client) {
    auto send = [client, ns](BSONObj cmd) {
        BSONObj res;
        client->runCommand(ns.dbName(), cmd, res);
        return getStatusFromCommandResult(res);
    };

    return send;
}

SessionsCollection::FindBatchFn SessionsCollection::makeFindFnForCommand(const NamespaceString& ns,
                                                                         DBClientBase* client) {
    auto send = [client, ns](BSONObj cmd) -> BSONObj {
        BSONObj res;
        if (!client->runCommand(ns.dbName(), cmd, res)) {
            uassertStatusOK(getStatusFromCommandResult(res));
        }

        return res;
    };

    return send;
}

SessionsCollection::RefreshSessionsResult SessionsCollection::_doRefresh(
    const NamespaceString& ns,
    const std::vector<LogicalSessionRecord>& sessions,
    SendBatchFn send) {
    // Used to refresh items from the session collection with write concern majority
    const WriteConcernOptions kMajorityWriteConcern{
        WriteConcernOptions::kMajority,
        WriteConcernOptions::SyncMode::UNSET,
        Milliseconds(mongo::gSessionWriteConcernTimeoutSystemMillis.load())};

    auto init = [ns, kMajorityWriteConcern](BSONObjBuilder* batch) {
        batch->append("update", ns.coll());
        batch->append("ordered", false);
        batch->append(WriteConcernOptions::kWriteConcernField, kMajorityWriteConcern.toBSON());
    };

    auto add = [](BSONArrayBuilder* entries, const LogicalSessionRecord& record) {
        entries->append(
            BSON("q" << lsidQuery(record) << "u" << updateQuery(record) << "upsert" << true));
    };

    auto result = runBulkCmd("updates", init, add, send, sessions);
    LogicalSessionRecordSet failedSessions;
    for (size_t idx : result.failedIndices) {
        if (idx < sessions.size()) {
            failedSessions.insert(sessions[idx]);
        }
    }
    return {failedSessions, result.errors};
}

void SessionsCollection::_doRemove(const NamespaceString& ns,
                                   const std::vector<LogicalSessionId>& sessions,
                                   SendBatchFn send) {
    // Used to remove items from the session collection with write concern majority
    const WriteConcernOptions kMajorityWriteConcern{
        WriteConcernOptions::kMajority,
        WriteConcernOptions::SyncMode::UNSET,
        Milliseconds(mongo::gSessionWriteConcernTimeoutSystemMillis.load())};

    auto init = [ns, kMajorityWriteConcern](BSONObjBuilder* batch) {
        batch->append("delete", ns.coll());
        batch->append("ordered", false);
        batch->append(WriteConcernOptions::kWriteConcernField, kMajorityWriteConcern.toBSON());
    };

    auto add = [](BSONArrayBuilder* builder, const LogicalSessionId& lsid) {
        builder->append(BSON("q" << lsidQuery(lsid) << "limit" << 1));
    };

    auto result = runBulkCmd("deletes", init, add, send, sessions);
    if (result.hasErrors()) {
        uassertStatusOK(result.errors[0]);
    }
}

LogicalSessionIdSet SessionsCollection::_doFindRemoved(
    const NamespaceString& ns, const std::vector<LogicalSessionId>& sessions, FindBatchFn send) {
    auto makeT = [] {
        return std::vector<LogicalSessionId>{};
    };

    auto add = [](std::vector<LogicalSessionId>& batch, const LogicalSessionId& record) {
        batch.push_back(record);
    };

    LogicalSessionIdSet removed{sessions.begin(), sessions.end()};

    auto wrappedSend = [&](BSONObj batch) {
        BSONObjBuilder batchWithReadConcernLocal(batch);
        batchWithReadConcernLocal.append(repl::ReadConcernArgs::kReadConcernFieldName,
                                         repl::ReadConcernArgs::kLocal.toBSONInner());
        auto swBatchResult = send(batchWithReadConcernLocal.obj());

        auto result = SessionsCollectionFetchResult::parse(
            swBatchResult, IDLParserContext{"SessionsCollectionFetchResult"});

        for (const auto& lsid : result.getCursor().getFirstBatch()) {
            removed.erase(lsid.get_id());
        }
    };

    auto sendLocal = [&](std::vector<LogicalSessionId>& batch) {
        SessionsCollectionFetchRequest request;

        request.setFind(ns.coll());
        request.setFilter({});
        request.getFilter().set_id({});
        request.getFilter().get_id().setIn(batch);

        request.setProjection({});
        request.getProjection().set_id(1);
        request.setBatchSize(batch.size());
        request.setLimit(batch.size());
        request.setSingleBatch(true);

        wrappedSend(request.toBSON());
        return Status::OK();
    };

    auto result = runBulkGeneric(makeT, add, sendLocal, sessions);
    if (result.hasErrors()) {
        uassertStatusOK(result.errors[0]);
    }

    return removed;
}

BSONObj SessionsCollection::generateCreateIndexesCmd() {
    NewIndexSpec index;
    index.setKey(BSON("lastUse" << 1));
    index.setName(kSessionsTTLIndex);
    index.setExpireAfterSeconds(localLogicalSessionTimeoutMinutes * 60);

    CreateIndexesCommand createIndexes(NamespaceString::kLogicalSessionsNamespace);
    createIndexes.setIndexes({index.toBSON()});
    createIndexes.setWriteConcern(WriteConcernOptions());
    return createIndexes.toBSON();
}

BSONObj SessionsCollection::generateCollModCmd() {
    BSONObjBuilder collModCmdBuilder;

    collModCmdBuilder << "collMod" << NamespaceString::kLogicalSessionsNamespace.coll();

    BSONObjBuilder indexBuilder(collModCmdBuilder.subobjStart("index"));
    indexBuilder << "name" << kSessionsTTLIndex;
    indexBuilder << "expireAfterSeconds" << localLogicalSessionTimeoutMinutes * 60;

    indexBuilder.done();
    collModCmdBuilder.append(WriteConcernOptions::kWriteConcernField,
                             WriteConcernOptions::kInternalWriteDefault);
    collModCmdBuilder.done();

    return collModCmdBuilder.obj();
}

}  // namespace mongo
