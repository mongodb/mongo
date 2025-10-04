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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"

#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {
static constexpr StringData kErrorLabelsFieldName = "errorLabels"_sd;
namespace ErrorLabel {
// PLEASE CONSULT DRIVERS BEFORE ADDING NEW ERROR LABELS.
constexpr inline auto kTransientTransaction = "TransientTransactionError"_sd;
constexpr inline auto kRetryableWrite = "RetryableWriteError"_sd;
constexpr inline auto kNonResumableChangeStream = "NonResumableChangeStreamError"_sd;
constexpr inline auto kResumableChangeStream = "ResumableChangeStreamError"_sd;
constexpr inline auto kNoWritesPerformed = "NoWritesPerformed"_sd;
constexpr inline auto kStreamProcessorRetryableError = "StreamProcessorRetryableError"_sd;
constexpr inline auto kStreamProcessorUserError = "StreamProcessorUserError"_sd;
constexpr inline auto kSystemOverloadedError = "SystemOverloadedError"_sd;
constexpr inline auto kRetryableError = "RetryableError"_sd;

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

}  // namespace mongo
