// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_compiled_configuration.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_error_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"

#include <string>

#include <wiredtiger.h>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
using NoReadTimestamp = WiredTigerBeginTxnBlock::NoReadTimestamp;

static inline int getConfigOffset(int ignore_prepare,
                                  int roundup_prepared,
                                  int roundup_read,
                                  int no_timestamp) {
    static constexpr int roundup_read_factor = static_cast<int>(NoReadTimestamp::kMax);
    static constexpr int roundup_prepared_factor =
        static_cast<int>(RoundUpReadTimestamp::kMax) * roundup_read_factor;
    static constexpr int ignore_prepare_factor =
        static_cast<int>(RoundUpPreparedTimestamps::kMax) * roundup_prepared_factor;
    return ignore_prepare * ignore_prepare_factor + roundup_prepared * roundup_prepared_factor +
        roundup_read * roundup_read_factor + no_timestamp;
}

static std::vector<CompiledConfiguration>& makeCompiledConfigurations() {
    static std::vector<CompiledConfiguration> compiledConfigurations;
    std::string ignore_prepare_str[] = {"false", "true", "force"};
    std::string false_true_str[] = {"false", "true"};
    for (int ignore_prepare = static_cast<int>(PrepareConflictBehavior::kEnforce);
         ignore_prepare < static_cast<int>(PrepareConflictBehavior::kMax);
         ignore_prepare++)
        for (int roundup_prepared = static_cast<int>(RoundUpPreparedTimestamps::kNoRound);
             roundup_prepared < static_cast<int>(RoundUpPreparedTimestamps::kMax);
             roundup_prepared++)
            for (int roundup_read = static_cast<int>(RoundUpReadTimestamp::kNoRoundError);
                 roundup_read < static_cast<int>(RoundUpReadTimestamp::kMax);
                 roundup_read++)
                for (int no_timestamp = static_cast<int>(NoReadTimestamp::kFalse);
                     no_timestamp < static_cast<int>(NoReadTimestamp::kMax);
                     no_timestamp++) {
                    // not to compile default
                    int config = getConfigOffset(
                        ignore_prepare, roundup_prepared, roundup_read, no_timestamp);
                    if (config == 0) {
                        continue;
                    }
                    const std::string beginTxnConfigString = fmt::format(
                        "ignore_prepare={},roundup_timestamps=(prepared={},read={}),no_timestamp={"
                        "}",
                        ignore_prepare_str[ignore_prepare],
                        false_true_str[roundup_prepared],
                        false_true_str[roundup_read],
                        false_true_str[no_timestamp]);
                    compiledConfigurations.emplace_back("WT_SESSION.begin_transaction",
                                                        beginTxnConfigString.c_str());
                }
    return compiledConfigurations;
}

static std::vector<CompiledConfiguration>& compiledBeginTransactions = makeCompiledConfigurations();

WiredTigerBeginTxnBlock::WiredTigerBeginTxnBlock(
    WiredTigerSession* session,
    PrepareConflictBehavior prepareConflictBehavior,
    bool roundUpPreparedTimestamps,
    RoundUpReadTimestamp roundUpReadTimestamp,
    RecoveryUnit::UntimestampedWriteAssertionLevel allowUntimestampedWrite,
    boost::optional<uint64_t> claimPreparedId)
    : _session(session) {
    invariant(!_rollback);

    NoReadTimestamp no_timestamp = NoReadTimestamp::kFalse;
    if (allowUntimestampedWrite != RecoveryUnit::UntimestampedWriteAssertionLevel::kEnforce) {
        no_timestamp = NoReadTimestamp::kTrue;
    }

    int config = getConfigOffset(static_cast<int>(prepareConflictBehavior),
                                 static_cast<int>(roundUpPreparedTimestamps
                                                      ? RoundUpPreparedTimestamps::kRound
                                                      : RoundUpPreparedTimestamps::kNoRound),
                                 static_cast<int>(roundUpReadTimestamp),
                                 static_cast<int>(no_timestamp));
    const char* compiled_config = nullptr;
    if (config > 0) {
        compiled_config = compiledBeginTransactions[config - 1].getConfig(_session);
    }

    if (claimPreparedId) {
        // Slow path used on startup recovery to take ownership of a prepared transaction as part of
        // the transaction.
        std::stringstream rawConfig;
        if (config > 0) {
            // We need to get the raw config because the compiled config is not a human readable
            // string that allows us to concatenate the claim_prepared_id.
            rawConfig << compiledBeginTransactions[config - 1].getRawConfig() << ",";
        }
        rawConfig << fmt::format("claim_prepared_id={}", unsignedHex(*claimPreparedId));
        std::string rawConfigStr = rawConfig.str();
        invariantWTOK(_session->begin_transaction(rawConfigStr.c_str()), *_session);
    } else {
        invariantWTOK(_session->begin_transaction(compiled_config), *_session);
    }

    _rollback = true;
}

WiredTigerBeginTxnBlock::WiredTigerBeginTxnBlock(WiredTigerSession* session, const char* config)
    : _session(session) {
    invariant(!_rollback);
    invariantWTOK(_session->begin_transaction(config), *_session);
    _rollback = true;
}

WiredTigerBeginTxnBlock::~WiredTigerBeginTxnBlock() {
    if (_rollback) {
        invariant(_session->rollback_transaction(nullptr) == 0);
    }
}

Status WiredTigerBeginTxnBlock::setReadSnapshot(Timestamp readTimestamp) {
    invariant(_rollback);
    return wtRCToStatus(
        _session->timestamp_transaction_uint(WT_TS_TXN_TYPE_READ, readTimestamp.asULL()),
        *_session);
}

void WiredTigerBeginTxnBlock::done() {
    invariant(_rollback);
    _rollback = false;
}

}  // namespace mongo
