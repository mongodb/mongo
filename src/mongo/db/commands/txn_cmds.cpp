// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"

#include <memory>
#include <set>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(participantReturnNetworkErrorForAbortAfterExecutingAbortLogic);
MONGO_FAIL_POINT_DEFINE(participantReturnNetworkErrorForCommitAfterExecutingCommitLogic);
MONGO_FAIL_POINT_DEFINE(hangBeforeCommitingTxn);
MONGO_FAIL_POINT_DEFINE(hangBeforeAbortingTxn);
// TODO SERVER-39704: Remove this fail point once the router can safely retry within a transaction
// on stale version and snapshot errors.
MONGO_FAIL_POINT_DEFINE(dontRemoveTxnCoordinatorOnAbort);

class CmdCommitTxn final : public CommitTransactionCmdVersion1Gen<CmdCommitTxn> {
public:
    CmdCommitTxn() = default;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const final {
        return true;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    std::string help() const final {
        return "Commits a transaction";
    }

    bool isTransactionCommand() const final {
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    bool requiresAuthzChecks() const override {
        return false;
    }
    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        Reply typedRun(OperationContext* opCtx) final {
            auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(ErrorCodes::CommandFailed,
                    "commitTransaction must be run within a transaction",
                    txnParticipant);

            const TxnNumberAndRetryCounter txnNumberAndRetryCounter{*opCtx->getTxnNumber(),
                                                                    *opCtx->getTxnRetryCounter()};

            LOGV2_DEBUG(20507,
                        3,
                        "Received commitTransaction",
                        "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter,
                        "sessionId"_attr = opCtx->getLogicalSessionId()->toBSON());

            // commitTransaction is retryable.
            if (txnParticipant.transactionIsCommitted()) {
                // We set the client last op to the last optime observed by the system to ensure
                // that we wait for the specified write concern on an optime greater than or equal
                // to the commit oplog entry.
                auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
                replClient.setLastOpToSystemLastOpTime(opCtx);
                if (MONGO_unlikely(participantReturnNetworkErrorForCommitAfterExecutingCommitLogic
                                       .shouldFail())) {
                    uasserted(ErrorCodes::HostUnreachable,
                              "returning network error because failpoint is on");
                }

                return Reply();
            }

            uassert(ErrorCodes::NoSuchTransaction,
                    "Transaction isn't in progress",
                    txnParticipant.transactionIsOpen());
            hangBeforeCommitingTxn.executeIf(
                [&](const BSONObj& data) {
                    CurOpFailpointHelpers::waitWhileFailPointEnabled(
                        &hangBeforeCommitingTxn, opCtx, "hangBeforeCommitingTxn");
                },
                [&](const BSONObj& data) {
                    // Hang if we don't provide a txnNumber to the fail point. If we do, only hang
                    // if the txnNumber matches the current running txnNumber.
                    if (data.hasField("uuid")) {
                        auto uuid = UUID::parse(data);
                        return uuid == opCtx->getLogicalSessionId()->getId();
                    }
                    return true;
                });

            auto optionalCommitTimestamp = request().getCommitTimestamp();
            if (optionalCommitTimestamp) {
                // commitPreparedTransaction will throw if the transaction is not prepared.
                txnParticipant.commitPreparedTransaction(
                    opCtx, optionalCommitTimestamp.value(), {});
            } else {
                if (auto role = ShardingState::get(opCtx)->pollClusterRole(); role &&
                    (role->has(ClusterRole::ConfigServer) || role->has(ClusterRole::ShardServer))) {
                    TransactionCoordinatorService::get(opCtx)->cancelIfCommitNotYetStarted(
                        opCtx, *opCtx->getLogicalSessionId(), txnNumberAndRetryCounter);
                }

                // commitUnpreparedTransaction will throw if the transaction is prepared.
                txnParticipant.commitUnpreparedTransaction(opCtx);
            }

            if (MONGO_unlikely(
                    participantReturnNetworkErrorForCommitAfterExecutingCommitLogic.shouldFail())) {
                uasserted(ErrorCodes::HostUnreachable,
                          "returning network error because failpoint is on");
            }

            return Reply();
        }
    };
};
MONGO_REGISTER_COMMAND(CmdCommitTxn).forShard();

static const Status kOnlyTransactionsReadConcernsSupported{
    ErrorCodes::InvalidOptions, "only read concerns valid in transactions are supported"};
static const Status kDefaultReadConcernNotPermitted{ErrorCodes::InvalidOptions,
                                                    "default read concern not permitted"};

class CmdAbortTxn final : public AbortTransactionCmdVersion1Gen<CmdAbortTxn> {
public:
    CmdAbortTxn() = default;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const final {
        return true;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    std::string help() const final {
        return "Aborts a transaction";
    }

    bool isTransactionCommand() const final {
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    bool requiresAuthzChecks() const override {
        return false;
    }
    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return true;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const final {
            // abortTransaction commences running inside a transaction (even though the transaction
            // will be ended by the time it completes).  Therefore it needs to accept any
            // readConcern which is valid within a transaction.  However it is not appropriate to
            // apply the default readConcern, since the readConcern of the transaction (set by the
            // first operation) is what must apply.
            return {{!isReadConcernLevelAllowedInTransaction(level),
                     kOnlyTransactionsReadConcernsSupported},
                    {kDefaultReadConcernNotPermitted}};
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        Reply typedRun(OperationContext* opCtx) final {
            auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(ErrorCodes::CommandFailed,
                    "abortTransaction must be run within a transaction",
                    txnParticipant);

            const TxnNumberAndRetryCounter txnNumberAndRetryCounter{*opCtx->getTxnNumber(),
                                                                    *opCtx->getTxnRetryCounter()};

            LOGV2_DEBUG(20508,
                        3,
                        "Received abortTransaction",
                        "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter,
                        "sessionId"_attr = opCtx->getLogicalSessionId()->toBSON());

            uassert(ErrorCodes::NoSuchTransaction,
                    "Transaction isn't in progress",
                    txnParticipant.transactionIsOpen());

            CurOpFailpointHelpers::waitWhileFailPointEnabled(
                &hangBeforeAbortingTxn, opCtx, "hangBeforeAbortingTxn");

            if (!MONGO_unlikely(dontRemoveTxnCoordinatorOnAbort.shouldFail())) {
                if (auto role = ShardingState::get(opCtx)->pollClusterRole(); role &&
                    (role->has(ClusterRole::ConfigServer) || role->has(ClusterRole::ShardServer))) {
                    TransactionCoordinatorService::get(opCtx)->cancelIfCommitNotYetStarted(
                        opCtx, *opCtx->getLogicalSessionId(), txnNumberAndRetryCounter);
                }
            }
            // Instruct the storage engine to not do any extra eviction while aborting transactions
            // so that resources will not get stuck.
            shard_role_details::getRecoveryUnit(opCtx)->setNoEvictionAfterCommitOrRollback();
            txnParticipant.abortTransaction(opCtx);

            if (MONGO_unlikely(
                    participantReturnNetworkErrorForAbortAfterExecutingAbortLogic.shouldFail())) {
                uasserted(ErrorCodes::HostUnreachable,
                          "returning network error because failpoint is on");
            }

            return Reply();
        }
    };
};
MONGO_REGISTER_COMMAND(CmdAbortTxn).forShard();

}  // namespace
}  // namespace mongo
