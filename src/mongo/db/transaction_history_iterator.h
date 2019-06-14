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

#pragma once

#include "mongo/db/repl/oplog_entry.h"

namespace mongo {

class OperationContext;

/**
 * An iterator class that traverses backwards through a transaction's oplog entries by following the
 * "prevOpTime" link in each entry.
 */
class TransactionHistoryIteratorBase {
public:
    virtual ~TransactionHistoryIteratorBase() = default;

    /**
     * Returns false if there are no more entries to iterate.
     */
    virtual bool hasNext() const = 0;

    /**
     * Returns an oplog entry and advances the iterator one step back through the oplog.
     * Should not be called if hasNext is false.
     * Throws if next oplog entry is in a unrecognized format or if it can't find the next oplog
     * entry.
     */
    virtual repl::OplogEntry next(OperationContext* opCtx) = 0;

    /**
     * Same as next() but returns only the OpTime, instead of the entire entry.
     */
    virtual repl::OpTime nextOpTime(OperationContext* opCtx) = 0;
};

class TransactionHistoryIterator : public TransactionHistoryIteratorBase {
public:
    /**
     * Creates a new iterator starting with an oplog entry with the given start opTime.
     */
    TransactionHistoryIterator(repl::OpTime startingOpTime, bool permitYield = false);
    virtual ~TransactionHistoryIterator() = default;

    bool hasNext() const override;
    repl::OplogEntry next(OperationContext* opCtx) override;
    repl::OpTime nextOpTime(OperationContext* opCtx) override;

    /**
     * Same as next() but makes exceptions fatal.
     */
    repl::OplogEntry nextFatalOnErrors(OperationContext* opCtx);

private:
    // Clients can set this to allow PlanExecutors created by this TransactionHistoryIterator to
    // have a YIELD_AUTO yield policy. It is only safe to set this if next() will never be called
    // while holding a lock that should not be yielded, such as the PBWM lock.
    bool _permitYield;

    repl::OpTime _nextOpTime;
};

}  // namespace mongo
