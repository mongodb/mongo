// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/get_status_from_command_result_write_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <variant>
#include <vector>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
using executor::RemoteCommandResponse;

enum class CommandErrorProvenance { kLocal, kRemote };

/**
 * Contains information to augment the 'RemoteCommandExecutionError' error code. In particular, this
 * class holds the provenance and data of the underlying error(s).
 */
class AsyncRPCErrorInfo final : public ErrorExtraInfo {
public:
    /**
     * Nested class used to describe remote errors.
     */
    class RemoteError {
    public:
        // The BSON received over-the-wire that encodes the remote
        // error is stored and used to construct this in-memory representation.
        explicit RemoteError(RemoteCommandResponse rcr)
            : _error{rcr.data},
              _remoteCommandResult{getStatusFromCommandResult(_error)},
              _remoteCommandWriteConcernError{getWriteConcernStatusFromCommandResult(_error)},
              _remoteCommandFirstWriteError{getFirstWriteErrorStatusFromCommandResult(_error)},
              _targetUsed{rcr.target},
              _elapsed{*rcr.elapsed} {
            // The buffer backing the default empty BSONObj has static duration so it is effectively
            // owned.
            invariant(_error.isOwned() || _error.objdata() == BSONObj().objdata());
            if (BSONElement errLabelsElem = _error["errorLabels"]; !errLabelsElem.eoo()) {
                _errLabels = errLabelsElem.Array();
            }
            _genericReplyFields = GenericReplyFields::parseSharingOwnership(
                _error, IDLParserContext("AsyncRPCRunner"));
        }

        Status getRemoteCommandResult() const {
            return _remoteCommandResult;
        }

        Status getRemoteCommandWriteConcernError() const {
            return _remoteCommandWriteConcernError;
        }

        Status getRemoteCommandFirstWriteError() const {
            return _remoteCommandFirstWriteError;
        }

        BSONObj getResponseObj() const {
            return _error;
        }

        std::vector<BSONElement> getErrorLabels() const {
            return _errLabels;
        }

        HostAndPort getTargetUsed() const {
            return _targetUsed;
        }

        Microseconds getElapsed() const {
            return _elapsed;
        }

        GenericReplyFields getGenericReplyFields() const {
            return _genericReplyFields;
        }

    private:
        BSONObj _error;
        Status _remoteCommandResult;
        Status _remoteCommandWriteConcernError;
        Status _remoteCommandFirstWriteError;
        std::vector<BSONElement> _errLabels;
        HostAndPort _targetUsed;
        Microseconds _elapsed;
        GenericReplyFields _genericReplyFields;
    };

    // Required member of every ErrorExtraInfo.
    static constexpr auto code = ErrorCodes::RemoteCommandExecutionError;

    // Unused and marked unreachable - the RemoteCommandExecutionError is InternalOnly and should
    // never be encoded in a BSONObj / received or sent over-the-wire.
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

    /**
     * Construct the relevant extra info from the RemoteCommandResponse provided by the TaskExecutor
     * used to invoke the remote command.
     */
    AsyncRPCErrorInfo(RemoteCommandResponse rcr)
        : _prov{[&] {
              if (!rcr.status.isOK())
                  return CommandErrorProvenance::kLocal;
              return CommandErrorProvenance::kRemote;
          }()},
          _error{[&]() -> std::variant<Status, RemoteError> {
              if (_prov == CommandErrorProvenance::kLocal) {
                  return rcr.status;
              } else {
                  return RemoteError(rcr);
              }
          }()},
          _targetAttempted{rcr.target} {}
    /**
     * Construct the relevant extra info from an error status - used if a remote command invokation
     * attempt fails before it reaches the TaskExecutor level.
     */
    explicit AsyncRPCErrorInfo(Status s) : _prov{CommandErrorProvenance::kLocal}, _error{s} {}

    AsyncRPCErrorInfo(Status s, boost::optional<HostAndPort> target)
        : _prov{CommandErrorProvenance::kLocal}, _error{s}, _targetAttempted{target} {}

    bool isLocal() const {
        return _prov == CommandErrorProvenance::kLocal;
    }

    bool isRemote() const {
        return _prov == CommandErrorProvenance::kRemote;
    }

    const RemoteError& asRemote() const {
        return get<RemoteError>(_error);
    }

    Status asLocal() const {
        return get<Status>(_error);
    }

    boost::optional<HostAndPort> getTargetAttempted() const {
        return _targetAttempted;
    }

    void setTargetAttempted(boost::optional<HostAndPort> targetAttempted) {
        _targetAttempted = targetAttempted;
    }

    // Unused and marked unreachable - the RemoteCommandExecutionError is InternalOnly and should
    // never be encoded in a BSONObj / received or sent over-the-wire.
    void serialize(BSONObjBuilder* bob) const final;

private:
    AsyncRPCErrorInfo(RemoteError err) : _prov{CommandErrorProvenance::kRemote}, _error{err} {}
    CommandErrorProvenance _prov;
    std::variant<Status, RemoteError> _error;
    boost::optional<HostAndPort> _targetAttempted;
};

namespace async_rpc {
/**
 * Note! Treats *all* possible RPC failures, including writeConcern and write errors,
 * as errors!
 *
 * Converts a RemoteCommandExecutionError from the async_rpc::sendCommand API
 * into the highest-priority 'underlying error' responsible for the RPC error.
 * The priority-ordering is as follows:
 *   (1) If there was an error on the local node (i.e. the sender) that caused
 *       the failure, that error is returned.
 *   (2) If we received an {ok: 0} response from the remote node, that error
 *       is returned.
 *   (3) If we received an {ok: 1} response from the remote node, but a write
 *       concern error, the write concern error is returned.
 *   (4) If we received an {ok: 1} response from the remote node, and no write
 *       concern error, but write error[s], the first write error is returned.
 */
Status unpackRPCStatus(Status status);

/**
 * Converts a RemoteCommandExecutionError from the async_rpc::sendCommand API
 * into the highest-priority 'underlying error' responsible for the RPC error,
 * ignoring write errors returned from the remote. This means:
 *   (1) If there was an error on the local node that caused the failure,
 *       that error is returned.
 *   (2) If we received an {ok: 0} response from the remote node, that error
 *       is returned.
 *   (3) If we received an {ok: 1} response from the remote node, but a write
 *       concern error, the write concern error is returned.
 */
Status unpackRPCStatusIgnoringWriteErrors(Status status);

/**
 * Converts a RemoteCommandExecutionError from the async_rpc::sendCommand API
 * into the highest-priority 'underlying error' responsible for the RPC error,
 * ignoring and writeConcern and/or write errors returned from the remote. This
 * means:
 *   (1) If there was an error on the local node that caused the failure,
 *       that error is returned.
 *   (2) If we received an {ok: 0} response from the remote node, that error
 *       is returned.
 *   (3) Otherwise, we received an {ok: 1} response from the remote node,
 *       and therefore Status::OK is returned.
 */
Status unpackRPCStatusIgnoringWriteConcernAndWriteErrors(Status status);
};  // namespace async_rpc
}  // namespace mongo
