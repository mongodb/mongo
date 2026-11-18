// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/end_of_transaction_parameter.h"

#include "mongo/db/change_stream_metrics_util.h"
#include "mongo/db/end_of_transaction_parameter_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"

namespace mongo {

bool isAllowEndOfTransactionEnabled() {
    return gAllowEndOfTransaction.loadRelaxed();
}

bool isEndOfTransactionWritePathEnabled() {
    // The metadata is attached whenever the feature is enabled, without any FCV gating. It is safe
    // to send to a shard whose binary predates the metadata field: such a shard rejects the
    // 'commitTransaction' command with 'IDLUnknownField', the coordinator strips the metadata and
    // retries, and remembers the shard as ineligible in 'EndOfTransactionMetadataShardCache' so
    // later commits to it skip the metadata.
    return feature_flags::gFeatureFlagEndOfTransactionChangeEventSynthesis.isEnabled() &&
        isAllowEndOfTransactionEnabled();
}

Status onUpdateAllowEndOfTransaction(bool value) {
    change_stream::endOfTransactionAllowEndOfTransactionGauge().set(value ? 1 : 0);
    return Status::OK();
}

// Re-seeds the gauge when the global ServiceContext is created: the servers construct it right
// after otel::metrics::initialize(), which resets pre-existing metrics to their default.
ServiceContext::ConstructorActionRegisterer kSeedGaugeOnServiceContext(
    "EndOfTransactionAllowEndOfTransactionGaugeSeed", [](ServiceContext*) {
        change_stream::endOfTransactionAllowEndOfTransactionGauge().set(
            isAllowEndOfTransactionEnabled() ? 1 : 0);
    });

}  // namespace mongo
