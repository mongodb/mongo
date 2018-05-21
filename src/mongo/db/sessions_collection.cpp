/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/sessions_collection.h"

#include <memory>
#include <vector>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/create_indexes_gen.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/refresh_sessions_gen.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"

namespace mongo {

namespace {

// This batch size is chosen to ensure that we don't form requests larger than the 16mb limit.
// Especially for refreshes, the updates we send include the full user name (user@db), and user
// names can be quite large (we enforce a max 10k limit for usernames used with sessions).
//
// At 1000 elements, a 16mb payload gives us a budget of 16000 bytes per user, which we should
// comfortably be able to stay under, even with 10k user names.
constexpr size_t kMaxBatchSize = 1000;

// Used to refresh or remove items from the session collection with write
// concern majority
const BSONObj kMajorityWriteConcern = WriteConcernOptions(WriteConcernOptions::kMajority,
                                                          WriteConcernOptions::SyncMode::UNSET,
                                                          Seconds(15))
                                          .toBSON();


BSONObj lsidQuery(const LogicalSessionId& lsid) {
    return BSON(LogicalSessionRecord::kIdFieldName << lsid.toBSON());
}

BSONObj lsidQuery(const LogicalSessionRecord& record) {
    return lsidQuery(record.getId());
}

BSONObj updateQuery(const LogicalSessionRecord& record) {
    // { $max : { lastUse : <time> }, $setOnInsert : { user : <user> } }

    // Build our update doc.
    BSONObjBuilder updateBuilder;

    {
        BSONObjBuilder maxBuilder(updateBuilder.subobjStart("$currentDate"));
        maxBuilder.append(LogicalSessionRecord::kLastUseFieldName, true);
    }

    if (record.getUser()) {
        BSONObjBuilder setBuilder(updateBuilder.subobjStart("$setOnInsert"));
        setBuilder.append(LogicalSessionRecord::kUserFieldName, BSON("name" << *record.getUser()));
    }

    return updateBuilder.obj();
}

template <typename TFactory, typename AddLineFn, typename SendFn, typename Container>
Status runBulkGeneric(TFactory makeT, AddLineFn addLine, SendFn sendBatch, const Container& items) {
    using T = decltype(makeT());

    size_t i = 0;
    boost::optional<T> thing;

    auto setupBatch = [&] {
        i = 0;
        thing.emplace(makeT());
    };

    auto sendLocalBatch = [&] { return sendBatch(thing.value()); };

    setupBatch();

    for (const auto& item : items) {
        addLine(*thing, item);

        if (++i >= kMaxBatchSize) {
            auto res = sendLocalBatch();
            if (!res.isOK()) {
                return res;
            }

            setupBatch();
        }
    }

    if (i > 0) {
        return sendLocalBatch();
    } else {
        return Status::OK();
    }
}

template <typename InitBatchFn, typename AddLineFn, typename SendBatchFn, typename Container>
Status runBulkCmd(StringData label,
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
        initBatch(&(batchBuilder.get()));
        entries.emplace(batchBuilder->subarrayStart(label));

        return &(entries.get());
    };

    auto sendLocalBatch = [&](BSONArrayBuilder*) {
        entries->done();
        return sendBatch(batchBuilder->done());
    };

    return runBulkGeneric(makeBatch, addLine, sendLocalBatch, items);
}

}  // namespace

SessionsCollection::~SessionsCollection() = default;

SessionsCollection::SendBatchFn SessionsCollection::makeSendFnForBatchWrite(
    const NamespaceString& ns, DBClientBase* client) {
    auto send = [client, ns](BSONObj batch) -> Status {
        BSONObj res;
        if (!client->runCommand(ns.db().toString(), batch, res)) {
            return getStatusFromCommandResult(res);
        }

        BatchedCommandResponse response;
        std::string errmsg;
        if (!response.parseBSON(res, &errmsg)) {
            return {ErrorCodes::FailedToParse, errmsg};
        }

        return response.toStatus();
    };

    return send;
}

SessionsCollection::SendBatchFn SessionsCollection::makeSendFnForCommand(const NamespaceString& ns,
                                                                         DBClientBase* client) {
    auto send = [client, ns](BSONObj cmd) -> Status {
        BSONObj res;
        if (!client->runCommand(ns.db().toString(), cmd, res)) {
            return getStatusFromCommandResult(res);
        }

        return Status::OK();
    };

    return send;
}

SessionsCollection::FindBatchFn SessionsCollection::makeFindFnForCommand(const NamespaceString& ns,
                                                                         DBClientBase* client) {
    auto send = [client, ns](BSONObj cmd) -> StatusWith<BSONObj> {
        BSONObj res;
        if (!client->runCommand(ns.db().toString(), cmd, res)) {
            return getStatusFromCommandResult(res);
        }

        return res;
    };

    return send;
}

Status SessionsCollection::doRefresh(const NamespaceString& ns,
                                     const LogicalSessionRecordSet& sessions,
                                     SendBatchFn send) {
    auto init = [ns](BSONObjBuilder* batch) {
        batch->append("update", ns.coll());
        batch->append("ordered", false);
        batch->append("allowImplicitCollectionCreation", false);
        batch->append(WriteConcernOptions::kWriteConcernField, kMajorityWriteConcern);
    };

    auto add = [](BSONArrayBuilder* entries, const LogicalSessionRecord& record) {
        entries->append(
            BSON("q" << lsidQuery(record) << "u" << updateQuery(record) << "upsert" << true));
    };

    return runBulkCmd("updates", init, add, send, sessions);
}

Status SessionsCollection::doRefreshExternal(const NamespaceString& ns,
                                             const LogicalSessionRecordSet& sessions,
                                             SendBatchFn send) {
    auto makeT = [] { return std::vector<LogicalSessionRecord>{}; };

    auto add = [](std::vector<LogicalSessionRecord>& batch, const LogicalSessionRecord& record) {
        batch.push_back(record);
    };

    auto sendLocal = [&](std::vector<LogicalSessionRecord>& batch) {
        RefreshSessionsCmdFromClusterMember idl;
        idl.setRefreshSessionsInternal(batch);
        return send(idl.toBSON());
    };

    return runBulkGeneric(makeT, add, sendLocal, sessions);
}

Status SessionsCollection::doRemove(const NamespaceString& ns,
                                    const LogicalSessionIdSet& sessions,
                                    SendBatchFn send) {
    auto init = [ns](BSONObjBuilder* batch) {
        batch->append("delete", ns.coll());
        batch->append("ordered", false);
        batch->append(WriteConcernOptions::kWriteConcernField, kMajorityWriteConcern);
    };

    auto add = [](BSONArrayBuilder* builder, const LogicalSessionId& lsid) {
        builder->append(BSON("q" << lsidQuery(lsid) << "limit" << 0));
    };

    return runBulkCmd("deletes", init, add, send, sessions);
}

Status SessionsCollection::doRemoveExternal(const NamespaceString& ns,
                                            const LogicalSessionIdSet& sessions,
                                            SendBatchFn send) {
    // TODO SERVER-28335 Implement endSessions, with internal counterpart.
    return Status::OK();
}

StatusWith<LogicalSessionIdSet> SessionsCollection::doFetch(const NamespaceString& ns,
                                                            const LogicalSessionIdSet& sessions,
                                                            FindBatchFn send) {
    auto makeT = [] { return std::vector<LogicalSessionId>{}; };

    auto add = [](std::vector<LogicalSessionId>& batch, const LogicalSessionId& record) {
        batch.push_back(record);
    };

    LogicalSessionIdSet removed = sessions;

    auto wrappedSend = [&](BSONObj batch) {
        auto swBatchResult = send(batch);

        if (!swBatchResult.isOK()) {
            return swBatchResult.getStatus();
        } else {
            auto result = SessionsCollectionFetchResult::parse("SessionsCollectionFetchResult"_sd,
                                                               swBatchResult.getValue());

            for (const auto& lsid : result.getCursor().getFirstBatch()) {
                removed.erase(lsid.get_id());
            }

            return Status::OK();
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

        return wrappedSend(request.toBSON());
    };

    auto status = runBulkGeneric(makeT, add, sendLocal, sessions);

    if (!status.isOK()) {
        return status;
    }

    return removed;
}

BSONObj SessionsCollection::generateCreateIndexesCmd() {
    NewIndexSpec index;
    index.setKey(BSON("lastUse" << 1));
    index.setName("lsidTTLIndex");
    index.setExpireAfterSeconds(localLogicalSessionTimeoutMinutes * 60);

    std::vector<NewIndexSpec> indexes;
    indexes.push_back(std::move(index));

    CreateIndexesCmd createIndexes;
    createIndexes.setCreateIndexes(NamespaceString::kLogicalSessionsNamespace.coll());
    createIndexes.setIndexes(std::move(indexes));

    return createIndexes.toBSON();
}
}  // namespace mongo
