// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
using namespace std::literals::string_view_literals;
static constexpr std::string_view kErrorLabelsFieldName = "errorLabels"sv;
static constexpr std::string_view kBaseBackoffMSFieldName = "baseBackoffMS"sv;
namespace ErrorLabel {
// PLEASE CONSULT DRIVERS BEFORE ADDING NEW ERROR LABELS.
constexpr inline auto kTransientTransaction = "TransientTransactionError"sv;
constexpr inline auto kRetryableWrite = "RetryableWriteError"sv;
constexpr inline auto kNonResumableChangeStream = "NonResumableChangeStreamError"sv;
constexpr inline auto kResumableChangeStream = "ResumableChangeStreamError"sv;
constexpr inline auto kNoWritesPerformed = "NoWritesPerformed"sv;
constexpr inline auto kStreamProcessorRetryableError = "StreamProcessorRetryableError"sv;
constexpr inline auto kStreamProcessorUserError = "StreamProcessorUserError"sv;
constexpr inline auto kSystemOverloadedError = "SystemOverloadedError"sv;
constexpr inline auto kRetryableError = "RetryableError"sv;

}  // namespace ErrorLabel

class ErrorLabelBuilder {
public:
    ErrorLabelBuilder(OperationContext* opCtx,
                      const OperationSessionInfoFromClient& sessionOptions,
                      const std::string& commandName,
                      boost::optional<ErrorCodes::Error> code,
                      boost::optional<ErrorCodes::Error> wcCode,
                      bool isInternalClient,
                      bool isMongos,
                      bool isComingFromRouter,
                      const repl::OpTime& lastOpBeforeRun,
                      const repl::OpTime& lastOpAfterRun)
        : _opCtx(opCtx),
          _sessionOptions(sessionOptions),
          _commandName(commandName),
          _code(code),
          _wcCode(wcCode),
          _isInternalClient(isInternalClient),
          _isMongos(isMongos),
          _isComingFromRouter(isComingFromRouter),
          _lastOpBeforeRun(lastOpBeforeRun),
          _lastOpAfterRun(lastOpAfterRun) {}

    void build(BSONArrayBuilder& labels) const;

    bool isTransientTransactionError() const;
    bool isRetryableWriteError() const;
    bool isResumableChangeStreamError() const;
    bool isNonResumableChangeStreamError() const;
    bool isErrorWithNoWritesPerformed() const;
    bool isStreamProcessorUserError() const;
    bool isStreamProcessorRetryableError() const;
    bool isSystemOverloadedError() const;
    bool isOperationIdempotent() const;

private:
    bool _isCommitOrAbort() const;
    OperationContext* _opCtx;
    const OperationSessionInfoFromClient& _sessionOptions;
    const std::string& _commandName;
    boost::optional<ErrorCodes::Error> _code;
    boost::optional<ErrorCodes::Error> _wcCode;
    bool _isInternalClient;
    bool _isMongos;
    bool _isComingFromRouter;
    repl::OpTime _lastOpBeforeRun;
    repl::OpTime _lastOpAfterRun;
};

/**
 * Returns the error labels for the given error.
 *
 * The returned document may also include other fields related to the error labels that should be
 * appended to the final response returned to the client.
 */
BSONObj getErrorLabels(OperationContext* opCtx,
                       const OperationSessionInfoFromClient& sessionOptions,
                       const std::string& commandName,
                       boost::optional<ErrorCodes::Error> code,
                       boost::optional<ErrorCodes::Error> wcCode,
                       bool isInternalClient,
                       bool isMongos,
                       bool isComingFromRouter,
                       const repl::OpTime& lastOpBeforeRun,
                       const repl::OpTime& lastOpAfterRun);

/**
 * Whether a write error in a transaction should be labelled with "TransientTransactionError".
 */
bool isTransientTransactionError(ErrorCodes::Error code,
                                 bool hasWriteConcernError,
                                 bool isCommitOrAbort);

/**
 * Checks if an error reply has the TransientTransactionError label. We use this in cases where we
 * want to defer to whether a shard attached the label to an error it gave us.
 */
bool hasTransientTransactionErrorLabel(const ErrorReply& reply);

/**
 * Whether a stream processing error is retryable.
 */
bool isStreamProcessorRetryableError(ErrorCodes::Error code);

/**
 * Whether a stream processing error is a user error.
 */
bool isStreamProcessorUserError(ErrorCodes::Error code);

/**
 * Whether the error is caused by the system being overloaded.
 */
bool isSystemOverloadedError(ErrorCodes::Error code);

/**
 * Returns the configured `externalClientBaseBackoffMS` server parameter value in milliseconds, or 0
 * when disabled.
 */
long long getExternalClientBaseBackoffMS();

}  // namespace mongo
