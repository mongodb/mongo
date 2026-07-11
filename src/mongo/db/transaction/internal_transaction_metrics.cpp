// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/transaction/internal_transaction_metrics.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/util/decorable.h"

#include <memory>
#include <utility>


namespace mongo {


namespace {
const auto InternalTransactionsMetricsDecoration =
    ServiceContext::declareDecoration<InternalTransactionMetrics>();
}  // namespace

InternalTransactionMetrics* InternalTransactionMetrics::get(ServiceContext* service) {
    return &InternalTransactionsMetricsDecoration(service);
}

InternalTransactionMetrics* InternalTransactionMetrics::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

namespace {
class InternalTransactionsSSS : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    ~InternalTransactionsSSS() override = default;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        auto metrics = InternalTransactionMetrics::get(opCtx);

        BSONObjBuilder ret;
        ret.append("started", metrics->getStarted());
        ret.append("retriedTransactions", metrics->getRetriedTransactions());
        ret.append("retriedCommits", metrics->getRetriedCommits());
        ret.append("succeeded", metrics->getSucceeded());

        return ret.obj();
    }
};
auto& internalTransactionSSS =
    *ServerStatusSectionBuilder<InternalTransactionsSSS>("internalTransactions");
}  // namespace


}  // namespace mongo
