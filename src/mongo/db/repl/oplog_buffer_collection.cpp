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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/oplog_buffer_collection.h"

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/operation_context.h"

namespace mongo {
namespace repl {

namespace {

const char kDefaultOplogCollectionNamespace[] = "local.oplog.initialSyncTempBuffer";

}  // namespace

NamespaceString OplogBufferCollection::getDefaultNamespace() {
    return NamespaceString(kDefaultOplogCollectionNamespace);
}

OplogBufferCollection::OplogBufferCollection() : OplogBufferCollection(getDefaultNamespace()) {}

OplogBufferCollection::OplogBufferCollection(const NamespaceString& nss) : _nss(nss) {}

NamespaceString OplogBufferCollection::getNamespace() const {
    return _nss;
}

void OplogBufferCollection::startup(OperationContext* txn) {
    // TODO: use storage interface to create collection.
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        AutoGetOrCreateDb databaseWriteGuard(txn, _nss.db(), MODE_X);
        auto db = databaseWriteGuard.getDb();
        invariant(db);
        WriteUnitOfWork wuow(txn);
        auto coll = db->createCollection(txn, _nss.ns(), CollectionOptions());
        invariant(coll);
        wuow.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "OplogBufferCollection::startup", _nss.ns());
}

void OplogBufferCollection::shutdown(OperationContext* txn) {}

void OplogBufferCollection::pushEvenIfFull(OperationContext* txn, const Value& value) {}

void OplogBufferCollection::push(OperationContext* txn, const Value& value) {}

bool OplogBufferCollection::pushAllNonBlocking(OperationContext* txn,
                                               Batch::const_iterator begin,
                                               Batch::const_iterator end) {
    return false;
}

void OplogBufferCollection::waitForSpace(OperationContext* txn, std::size_t size) {}

bool OplogBufferCollection::isEmpty() const {
    return true;
}

std::size_t OplogBufferCollection::getMaxSize() const {
    return 0;
}

std::size_t OplogBufferCollection::getSize() const {
    return 0;
}

std::size_t OplogBufferCollection::getCount() const {
    return 0;
}

void OplogBufferCollection::clear(OperationContext* txn) {}

bool OplogBufferCollection::tryPop(OperationContext* txn, Value* value) {
    return false;
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
    return false;
}

boost::optional<OplogBuffer::Value> OplogBufferCollection::lastObjectPushed(
    OperationContext* txn) const {
    return {};
}

}  // namespace repl
}  // namespace mongo
