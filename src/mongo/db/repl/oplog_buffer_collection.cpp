/**
 *    Copyright (C) 2016 MongoDB Inc.
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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/oplog_buffer_collection.h"

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_and_modify_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batched_insert_request.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

namespace {

const char kDefaultOplogCollectionNamespace[] = "local.oplog.initialSyncTempBuffer";
const char kOplogEntryFieldName[] = "entry";

}  // namespace

NamespaceString OplogBufferCollection::getDefaultNamespace() {
    return NamespaceString(kDefaultOplogCollectionNamespace);
}

BSONObj OplogBufferCollection::addIdToDocument(const BSONObj& orig) {
    BSONObjBuilder bob;
    bob.append("_id", orig["ts"].timestamp());
    bob.append(kOplogEntryFieldName, orig);
    return bob.obj();
}

BSONObj OplogBufferCollection::extractEmbeddedOplogDocument(const BSONObj& orig) {
    return orig.getObjectField(kOplogEntryFieldName);
}


OplogBufferCollection::OplogBufferCollection() : OplogBufferCollection(getDefaultNamespace()) {}

OplogBufferCollection::OplogBufferCollection(const NamespaceString& nss)
    : _nss(nss), _count(0), _size(0) {}

NamespaceString OplogBufferCollection::getNamespace() const {
    return _nss;
}

void OplogBufferCollection::startup(OperationContext* txn) {
    _createCollection(txn);
}

void OplogBufferCollection::shutdown(OperationContext* txn) {
    _dropCollection(txn);
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _size = 0;
    _count = 0;
}

void OplogBufferCollection::pushEvenIfFull(OperationContext* txn, const Value& value) {
    Batch valueBatch = {value};
    pushAllNonBlocking(txn, valueBatch.begin(), valueBatch.end());
}

void OplogBufferCollection::push(OperationContext* txn, const Value& value) {
    pushEvenIfFull(txn, value);
}

bool OplogBufferCollection::pushAllNonBlocking(OperationContext* txn,
                                               Batch::const_iterator begin,
                                               Batch::const_iterator end) {
    size_t numDocs = 0;
    size_t docSize = 0;
    // TODO: use storage interface to insert documents.
    try {
        DBDirectClient client(txn);

        BatchedInsertRequest req;
        req.setNS(_nss);
        for (Batch::const_iterator it = begin; it != end; ++it) {
            req.addToDocuments(addIdToDocument(*it));
            numDocs++;
            docSize += it->objsize();
        }

        BSONObj res;
        client.runCommand(_nss.db().toString(), req.toBSON(), res);

        BatchedCommandResponse response;
        std::string errmsg;
        if (!response.parseBSON(res, &errmsg)) {
            return false;
        }

        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _count += numDocs;
        _size += docSize;
        return true;
    } catch (const DBException& e) {
        LOG(1) << "Pushing oplog entries to OplogBufferCollection failed with: " << e;
        return false;
    }
}

void OplogBufferCollection::waitForSpace(OperationContext* txn, std::size_t size) {}

bool OplogBufferCollection::isEmpty() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _count == 0;
}

std::size_t OplogBufferCollection::getMaxSize() const {
    return 0;
}

std::size_t OplogBufferCollection::getSize() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _size;
}

std::size_t OplogBufferCollection::getCount() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _count;
}

void OplogBufferCollection::clear(OperationContext* txn) {
    _dropCollection(txn);
    _createCollection(txn);
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _size = 0;
    _count = 0;
}

bool OplogBufferCollection::tryPop(OperationContext* txn, Value* value) {
    auto request = FindAndModifyRequest::makeRemove(_nss, BSONObj());
    request.setSort(BSON("_id" << 1));

    // TODO: use storage interface to find and remove document.
    try {
        DBDirectClient client(txn);
        BSONObj response;
        bool res = client.runCommand(_nss.db().toString(), request.toBSON(), response);
        if (!res) {
            return false;
        }

        if (auto okElem = response["ok"]) {
            if (!okElem.trueValue()) {
                return false;
            }
        }

        if (auto valueElem = response["value"]) {
            *value = extractEmbeddedOplogDocument(valueElem.Obj()).getOwned();
        } else {
            return false;
        }
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _count--;
        _size -= value->objsize();
        return true;
    } catch (const DBException& e) {
        LOG(1) << "Popping oplog entries from OplogBufferCollection failed with: " << e;
        return false;
    };
}

OplogBuffer::Value OplogBufferCollection::blockingPop(OperationContext* txn) {
    return {};
}

bool OplogBufferCollection::blockingPeek(OperationContext* txn,
                                         Value* value,
                                         Seconds waitDuration) {
    return false;
}

bool OplogBufferCollection::peek(OperationContext* txn, Value* value) {
    return _peekOneSide(txn, value, true);
}

boost::optional<OplogBuffer::Value> OplogBufferCollection::lastObjectPushed(
    OperationContext* txn) const {
    Value value;
    bool res = _peekOneSide(txn, &value, false);
    if (!res) {
        return boost::none;
    }
    return value;
}

bool OplogBufferCollection::_peekOneSide(OperationContext* txn, Value* value, bool front) const {
    int asc = front ? 1 : -1;
    // TODO: use storage interface for to find document.
    try {
        auto query = Query();
        query.sort("_id", asc);

        DBDirectClient client(txn);
        BSONObj response = client.findOne(_nss.ns(), query);
        if (response.isEmpty()) {
            return false;
        }
        *value = extractEmbeddedOplogDocument(response).getOwned();
        return true;
    } catch (const DBException& e) {
        LOG(1) << "Peeking oplog entries from OplogBufferCollection failed with: " << e;
        return false;
    }
}

void OplogBufferCollection::_createCollection(OperationContext* txn) {
    // TODO: use storage interface to create collection.
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        AutoGetOrCreateDb databaseWriteGuard(txn, _nss.db(), MODE_X);
        auto db = databaseWriteGuard.getDb();
        invariant(db);
        WriteUnitOfWork wuow(txn);
        CollectionOptions options;
        options.temp = true;
        auto coll = db->createCollection(txn, _nss.ns(), options);
        invariant(coll);
        wuow.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "OplogBufferCollection::_createCollection", _nss.ns());
}

void OplogBufferCollection::_dropCollection(OperationContext* txn) {
    // TODO: use storage interface to drop collection.
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        AutoGetOrCreateDb databaseWriteGuard(txn, _nss.db(), MODE_X);
        auto db = databaseWriteGuard.getDb();
        invariant(db);
        WriteUnitOfWork wuow(txn);
        db->dropCollection(txn, _nss.ns());
        wuow.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "OplogBufferCollection::_dropCollection", _nss.ns());
}

}  // namespace repl
}  // namespace mongo
