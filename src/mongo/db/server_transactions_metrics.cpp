/**
 *    Copyright (C) 2018 MongoDB, Inc.
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

#include "mongo/db/server_transactions_metrics.h"

#include "mongo/db/commands/server_status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/retryable_writes_stats.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transactions_stats_gen.h"

namespace mongo {
namespace {
const auto ServerTransactionsMetricsDecoration =
    ServiceContext::declareDecoration<ServerTransactionsMetrics>();
}  // namespace

ServerTransactionsMetrics* ServerTransactionsMetrics::get(ServiceContext* service) {
    return &ServerTransactionsMetricsDecoration(service);
}

ServerTransactionsMetrics* ServerTransactionsMetrics::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

unsigned long long ServerTransactionsMetrics::getTotalStarted() const {
    return _totalStarted.load();
}

void ServerTransactionsMetrics::incrementTotalStarted() {
    _totalStarted.fetchAndAdd(1);
}

unsigned long long ServerTransactionsMetrics::getTotalAborted() const {
    return _totalAborted.load();
}

void ServerTransactionsMetrics::incrementTotalAborted() {
    _totalAborted.fetchAndAdd(1);
}

unsigned long long ServerTransactionsMetrics::getTotalCommitted() const {
    return _totalCommitted.load();
}

void ServerTransactionsMetrics::incrementTotalCommitted() {
    _totalCommitted.fetchAndAdd(1);
}

void ServerTransactionsMetrics::updateStats(TransactionsStats* stats) {
    // This is a dummy function until we start tracking global transactions metrics.
}

class TransactionsSSS : public ServerStatusSection {
public:
    TransactionsSSS() : ServerStatusSection("transactions") {}

    virtual ~TransactionsSSS() {}

    virtual bool includeByDefault() const {
        return true;
    }

    virtual BSONObj generateSection(OperationContext* opCtx,
                                    const BSONElement& configElement) const {
        TransactionsStats stats;

        // Retryable writes and multi-document transactions metrics are both included in the same
        // serverStatus section because both utilize similar internal machinery for tracking their
        // lifecycle within a session. Both are assigned transaction numbers, and so both are often
        // referred to as “transactions”.
        RetryableWritesStats::get(opCtx)->updateStats(&stats);
        ServerTransactionsMetrics::get(opCtx)->updateStats(&stats);
        return stats.toBSON();
    }

} transactionsSSS;

}  // namespace mongo
