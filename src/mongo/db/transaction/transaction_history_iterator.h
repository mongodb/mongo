// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/modules.h"

#include <cstddef>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class OperationContext;

/**
 * An iterator class that traverses backwards through a transaction's oplog entries by following the
 * "prevOpTime" link in each entry.
 */
class [[MONGO_MOD_OPEN]] TransactionHistoryIteratorBase {
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

/**
 * Walks back through an applyOps chain, running 'step' (which fetches and processes one entry and
 * returns how many operations it consumed) until the chain ends or 'opsStillToCollect' reaches
 * zero. A transaction's chain is null-terminated, so it passes no budget and walks to the end. A
 * retryable batch's first entry instead links to the previous applyOps chain, so it passes the ops
 * left to collect; stopping on the count avoids reading a previous chain that may have been
 * truncated from the oplog. 'step' owns the fetch because callers differ over next(), nextOpTime()
 * and nextFatalOnErrors().
 */
template <typename StepFn>
void walkApplyOpsChain(TransactionHistoryIteratorBase& iter,
                       boost::optional<std::size_t> opsStillToCollect,
                       StepFn&& step) {
    while ((!opsStillToCollect || *opsStillToCollect > 0) && iter.hasNext()) {
        const std::size_t consumed = step();
        if (opsStillToCollect) {
            *opsStillToCollect = repl::remainingApplyOpsChainOps(*opsStillToCollect, consumed);
        }
    }
}

class TransactionHistoryIterator : public TransactionHistoryIteratorBase {
public:
    enum class IncludeCommitTimestamp { kYes, kNo };

    /**
     * Creates a new iterator starting with an oplog entry with the given start opTime.
     * TODO SERVER-104970: If permitYield can't be deleted, change the default to 'false'.
     */
    TransactionHistoryIterator(
        repl::OpTime startingOpTime,
        bool permitYield = true,
        IncludeCommitTimestamp includeCommitTimestamp = IncludeCommitTimestamp::kNo);

    ~TransactionHistoryIterator() override = default;

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
    // while holding a lock that should not be yielded.
    // TODO SERVER-104970: Determine whether this can be removed.
    bool _permitYield;

    // Whether the iterator should attach the commit timestamp to the oplog entries. Throws an error
    // if the commit timestamp is requested but this is not a committed transaction.
    IncludeCommitTimestamp _includeCommitTimestamp;

    repl::OpTime _nextOpTime;

    // The commit timestamp if this is the oplog chain for a committed transaction.
    Timestamp _commitTimestamp;
};

}  // namespace mongo
