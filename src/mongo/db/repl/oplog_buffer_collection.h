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

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/util/queue.h"

namespace mongo {
namespace repl {

/**
 * Oplog buffer backed by a temporary collection. This collection is created in startup() and
 * removed in shutdown().
 */
class OplogBufferCollection : public OplogBuffer {
public:
    /**
     * Returns default namespace for temporary collection used to hold data in oplog buffer.
     */
    static NamespaceString getDefaultNamespace();

    OplogBufferCollection();
    OplogBufferCollection(const NamespaceString& nss);

    /**
     * Returns the namespace string of the collection used by this oplog buffer.
     */
    NamespaceString getNamespace() const;

    void startup(OperationContext* txn) override;
    void shutdown(OperationContext* txn) override;
    void pushEvenIfFull(OperationContext* txn, const Value& value) override;
    void push(OperationContext* txn, const Value& value) override;
    bool pushAllNonBlocking(OperationContext* txn,
                            Batch::const_iterator begin,
                            Batch::const_iterator end) override;
    void waitForSpace(OperationContext* txn, std::size_t size) override;
    bool isEmpty(OperationContext* txn) const override;
    std::size_t getMaxSize() const override;
    std::size_t getSize(OperationContext* txn) const override;
    std::size_t getCount(OperationContext* txn) const override;
    void clear(OperationContext* txn) override;
    bool tryPop(OperationContext* txn, Value* value) override;
    Value blockingPop(OperationContext* txn) override;
    bool blockingPeek(OperationContext* txn, Value* value, Seconds waitDuration) override;
    bool peek(OperationContext* txn, Value* value) override;
    boost::optional<Value> lastObjectPushed(OperationContext* txn) const override;

private:
    const NamespaceString _nss;
};

}  // namespace repl
}  // namespace mongo
