/**
 *    Copyright (C) 2017 MongoDB, Inc.
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

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/logger/redaction.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

TransactionHistoryIterator::TransactionHistoryIterator(Timestamp startingOpTimeTs)
    : _nextOpTimeTs(std::move(startingOpTimeTs)) {}

bool TransactionHistoryIterator::hasNext() const {
    return !_nextOpTimeTs.isNull();
}

repl::OplogEntry TransactionHistoryIterator::next(OperationContext* opCtx) {
    invariant(hasNext());

    DBDirectClient client(opCtx);
    // TODO: SERVER-29843 oplogReplay option might be needed to activate fast ts search.
    auto oplogBSON =
        client.findOne(NamespaceString::kRsOplogNamespace.ns(),
                       BSON(repl::OplogEntryBase::kTimestampFieldName << _nextOpTimeTs),
                       /* fieldsToReturn */ nullptr,
                       DBClientCursor::QueryOptionLocal_forceOpQuery  // SERVER-30318
                       /* QueryOption_OplogReplay */);

    uassert(ErrorCodes::IncompleteTransactionHistory,
            str::stream() << "oplog no longer contains the complete write history of this "
                             "transaction, log with ts "
                          << _nextOpTimeTs.toBSON()
                          << " cannot be found",
            !oplogBSON.isEmpty());

    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(oplogBSON));
    const auto& oplogPrevTsOption = oplogEntry.getPrevWriteTsInTransaction();
    uassert(
        ErrorCodes::FailedToParse,
        str::stream() << "Missing prevTs field on oplog entry of previous write in transcation: "
                      << redact(oplogBSON),
        oplogPrevTsOption);

    _nextOpTimeTs = oplogPrevTsOption.value();

    return oplogEntry;
}

}  // namespace mongo
