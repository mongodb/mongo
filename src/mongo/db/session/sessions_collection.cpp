/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/session/sessions_collection.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/db/local_catalog/ddl/create_indexes_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/session/logical_session_cache_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/sessions_server_parameters_gen.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result_write_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <cstddef>
#include <cstdint>
#include <functional>
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

template <typename TFactory, typename AddLineFn, typename SendFn, typename Container>
void runBulkGeneric(TFactory makeT, AddLineFn addLine, SendFn sendBatch, const Container& items) {
    using T = decltype(makeT());

    size_t i = 0;
    boost::optional<T> thing;

    auto setupBatch = [&] {
        i = 0;
        thing.emplace(makeT());
    };

    auto sendLocalBatch = [&] {
        sendBatch(thing.value());
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
}

template <typename InitBatchFn, typename AddLineFn, typename SendBatchFn, typename Container>
void runBulkCmd(StringData label,
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
        sendBatch(batchBuilder->done());
    };

    runBulkGeneric(makeBatch, addLine, sendLocalBatch, items);
}

}  // namespace

constexpr StringData SessionsCollection::kSessionsTTLIndex;

SessionsCollection::SessionsCollection() = default;

SessionsCollection::~SessionsCollection() = default;

SessionsCollection::SendBatchFn SessionsCollection::makeSendFnForBatchWrite(
    const NamespaceString& ns, DBClientBase* client) {
    auto send = [client, ns](BSONObj batch) {
        BSONObj res;
        client->runCommand(ns.dbName(), batch, res);
        uassertStatusOK(getStatusFromWriteCommandReply(res));
    };

    return send;
}

SessionsCollection::SendBatchFn SessionsCollection::makeSendFnForCommand(const NamespaceString& ns,
                                                                         DBClientBase* client) {
    auto send = [client, ns](BSONObj cmd) {
        BSONObj res;
        if (!client->runCommand(ns.dbName(), cmd, res)) {
            uassertStatusOK(getStatusFromCommandResult(res));
        }
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

void SessionsCollection::_doRefresh(const NamespaceString& ns,
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

    runBulkCmd("updates", init, add, send, sessions);
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

    runBulkCmd("deletes", init, add, send, sessions);
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
    };

    runBulkGeneric(makeT, add, sendLocal, sessions);

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
