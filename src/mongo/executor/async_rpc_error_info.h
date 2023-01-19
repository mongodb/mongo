/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/generic_args_with_types_gen.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/variant.h"

namespace mongo {
using executor::RemoteCommandOnAnyResponse;

enum class CommandErrorProvenance { kLocal, kRemote };

/**
 * Contains generic reply fields that can be part of any command response, separated based on
 * whether fields are part of the stable API. The generic reply fields are generated from
 * '../idl/generic_args_with_types.idl'.
 */
struct GenericReplyFields {
    GenericReplyFields(
        GenericReplyFieldsWithTypesV1 stable = GenericReplyFieldsWithTypesV1(),
        GenericReplyFieldsWithTypesUnstableV1 unstable = GenericReplyFieldsWithTypesUnstableV1())
        : stable{stable}, unstable{unstable} {}
    GenericReplyFieldsWithTypesV1 stable;
    GenericReplyFieldsWithTypesUnstableV1 unstable;
};

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
        explicit RemoteError(RemoteCommandOnAnyResponse rcr)
            : _error{rcr.data},
              _remoteCommandResult{getStatusFromCommandResult(_error)},
              _remoteCommandWriteConcernError{getWriteConcernStatusFromCommandResult(_error)},
              _remoteCommandFirstWriteError{getFirstWriteErrorStatusFromCommandResult(_error)},
              _targetUsed{*rcr.target} {
            // The buffer backing the default empty BSONObj has static duration so it is effectively
            // owned.
            invariant(_error.isOwned() || _error.objdata() == BSONObj().objdata());
            if (BSONElement errLabelsElem = _error["errorLabels"]; !errLabelsElem.eoo()) {
                _errLabels = errLabelsElem.Array();
            }
            auto stableReplyFields = GenericReplyFieldsWithTypesV1::parseSharingOwnership(
                IDLParserContext("AsyncRPCRunner"), _error);
            auto unstableReplyFields = GenericReplyFieldsWithTypesUnstableV1::parseSharingOwnership(
                IDLParserContext("AsyncRPCRunner"), _error);
            _genericReplyFields = GenericReplyFields(stableReplyFields, unstableReplyFields);
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
    AsyncRPCErrorInfo(RemoteCommandOnAnyResponse rcr, std::vector<HostAndPort> targets)
        : _prov{[&] {
              if (!rcr.status.isOK())
                  return CommandErrorProvenance::kLocal;
              return CommandErrorProvenance::kRemote;
          }()},
          _error{[&]() -> stdx::variant<Status, RemoteError> {
              if (_prov == CommandErrorProvenance::kLocal) {
                  return rcr.status;
              } else {
                  return RemoteError(rcr);
              }
          }()},
          _targetsAttempted{targets} {}
    /**
     * Construct the relevant extra info from an error status - used if a remote command invokation
     * attempt fails before it reaches the TaskExecutor level.
     */
    explicit AsyncRPCErrorInfo(Status s) : _prov{CommandErrorProvenance::kLocal}, _error{s} {}

    AsyncRPCErrorInfo(Status s, std::vector<HostAndPort> targets)
        : _prov{CommandErrorProvenance::kLocal}, _error{s}, _targetsAttempted{targets} {}

    bool isLocal() const {
        return _prov == CommandErrorProvenance::kLocal;
    }

    bool isRemote() const {
        return _prov == CommandErrorProvenance::kRemote;
    }

    const RemoteError& asRemote() const {
        return stdx::get<RemoteError>(_error);
    }

    Status asLocal() const {
        return stdx::get<Status>(_error);
    }

    std::vector<HostAndPort> getTargetsAttempted() const {
        return _targetsAttempted;
    }

    void setTargetsAttempted(std::vector<HostAndPort> targetsAttempted) {
        _targetsAttempted = targetsAttempted;
    }

    // Unused and marked unreachable - the RemoteCommandExecutionError is InternalOnly and should
    // never be encoded in a BSONObj / received or sent over-the-wire.
    void serialize(BSONObjBuilder* bob) const final;

private:
    AsyncRPCErrorInfo(RemoteError err) : _prov{CommandErrorProvenance::kRemote}, _error{err} {}
    CommandErrorProvenance _prov;
    stdx::variant<Status, RemoteError> _error;
    std::vector<HostAndPort> _targetsAttempted;
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
