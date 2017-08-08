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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"

namespace mongo {

namespace {

BSONObj lsidQuery(const LogicalSessionId& lsid) {
    return BSON(LogicalSessionRecord::kIdFieldName << lsid.toBSON());
}

BSONObj lsidQuery(const LogicalSessionRecord& record) {
    return lsidQuery(record.getId());
}

BSONObj updateQuery(const LogicalSessionRecord& record, Date_t refreshTime) {
    // { $max : { lastUse : <time> }, $setOnInsert : { user : <user> } }

    // Build our update doc.
    BSONObjBuilder updateBuilder;

    {
        BSONObjBuilder maxBuilder(updateBuilder.subobjStart("$max"));
        maxBuilder.append(LogicalSessionRecord::kLastUseFieldName, refreshTime);
    }

    if (record.getUser()) {
        BSONObjBuilder setBuilder(updateBuilder.subobjStart("$setOnInsert"));
        setBuilder.append(LogicalSessionRecord::kUserFieldName, BSON("name" << *record.getUser()));
    }

    return updateBuilder.obj();
}

template <typename InitBatchFn, typename AddLineFn, typename SendBatchFn, typename Container>
Status runBulkCmd(StringData label,
                  InitBatchFn&& initBatch,
                  AddLineFn&& addLine,
                  SendBatchFn&& sendBatch,
                  const Container& items) {
    size_t i = 0;
    BufBuilder buf;

    boost::optional<BSONObjBuilder> batchBuilder;
    boost::optional<BSONArrayBuilder> entries;

    auto setupBatchBuilder = [&] {
        buf.reset();
        batchBuilder.emplace(buf);
        initBatch(&(batchBuilder.get()));
        entries.emplace(batchBuilder->subarrayStart(label));
    };

    auto sendLocalBatch = [&] {
        entries->done();
        return sendBatch(batchBuilder->done());
    };

    setupBatchBuilder();

    for (const auto& item : items) {
        addLine(&(entries.get()), item);

        if (++i >= write_ops::kMaxWriteBatchSize) {
            auto res = sendLocalBatch();
            if (!res.isOK()) {
                return res;
            }

            setupBatchBuilder();
            i = 0;
        }
    }

    if (i > 0) {
        return sendLocalBatch();
    } else {
        return Status::OK();
    }
}

}  // namespace


constexpr StringData SessionsCollection::kSessionsDb;
constexpr StringData SessionsCollection::kSessionsCollection;
constexpr StringData SessionsCollection::kSessionsFullNS;


SessionsCollection::~SessionsCollection() = default;

Status SessionsCollection::doRefresh(const LogicalSessionRecordSet& sessions,
                                     Date_t refreshTime,
                                     SendBatchFn send) {
    auto init = [](BSONObjBuilder* batch) {
        batch->append("update", kSessionsCollection);
        batch->append("ordered", false);
    };

    auto add = [&refreshTime](BSONArrayBuilder* entries, const LogicalSessionRecord& record) {
        entries->append(BSON("q" << lsidQuery(record) << "u" << updateQuery(record, refreshTime)
                                 << "upsert"
                                 << true));
    };

    return runBulkCmd("updates", init, add, send, sessions);
}

Status SessionsCollection::doRemove(const LogicalSessionIdSet& sessions, SendBatchFn send) {
    auto init = [](BSONObjBuilder* batch) {
        batch->append("delete", kSessionsCollection);
        batch->append("ordered", false);
    };

    auto add = [](BSONArrayBuilder* builder, const LogicalSessionId& lsid) {
        builder->append(BSON("q" << lsidQuery(lsid) << "limit" << 0));
    };

    return runBulkCmd("deletes", init, add, send, sessions);
}

}  // namespace mongo
