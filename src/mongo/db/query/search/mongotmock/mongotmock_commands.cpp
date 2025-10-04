/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/client_cursor/kill_cursors_gen.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/query/search/manage_search_index_request_gen.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/db/query/search/mongotmock/mongotmock_state.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(mongotWaitBeforeRespondingToQuery);

using mongotmock::CursorState;
using mongotmock::getMongotMockState;
using mongotmock::MongotMockStateGuard;


const BSONObj placeholderCmd = BSON("placeholder" << "expected");
const BSONObj placeholderResponse = BSON("placeholder" << "response");

void assertGivenCommandMatchesExpectedCommand(BSONObj givenCmd, BSONObj expectedCmd) {
    uassert(31086,
            str::stream() << "Expected command matching " << expectedCmd << " but got " << givenCmd,
            mongotmock::checkGivenCommandMatchesExpectedCommand(givenCmd, expectedCmd));
}

/**
 * Base class for MongotMock commands.
 */
class MongotMockBaseCmd : public BasicCommand {
public:
    MongotMockBaseCmd(StringData name) : BasicCommand(name, "") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        MONGO_UNREACHABLE;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        MONGO_UNREACHABLE;
    }

    std::string help() const final {
        MONGO_UNREACHABLE;
    }

    Status explain(OperationContext* opCtx,
                   const OpMsgRequest& request,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result) const final {
        MONGO_UNREACHABLE;
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const final {
        MONGO_UNREACHABLE;
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        processCommand(opCtx, dbName, cmdObj, &result);
        return true;
    }

    virtual void processCommand(OperationContext* opCtx,
                                const DatabaseName&,
                                const BSONObj& cmdObj,
                                BSONObjBuilder* result) const = 0;
};

class MongotMockCursorCommand : public MongotMockBaseCmd {
public:
    // This is not a real command, and thus must be built with a real command name.
    MongotMockCursorCommand() = delete;
    MongotMockCursorCommand(StringData commandName) : MongotMockBaseCmd(commandName) {}

private:
    void processCommand(OperationContext* opCtx,
                        const DatabaseName&,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result) const final {
        mongotWaitBeforeRespondingToQuery.pauseWhileSet();

        MongotMockStateGuard stateGuard = getMongotMockState(opCtx->getServiceContext());
        if (stateGuard->shouldCloseConnection()) {
            stateGuard->consumeCloseConnection();
            uasserted(
                ErrorCodes::StreamTerminated,
                str::stream()
                    << "Closing connection in response to search or planShardedSearchCommand");
        }
        CursorState* state = stateGuard->doOrderCheck() ? stateGuard->claimAvailableState()
                                                        : stateGuard->claimStateForCommand(cmdObj);
        uassert(31094,
                str::stream()
                    << "Cannot run search cursor command as there are no remaining unclaimed mock "
                       "cursor states. Received command: "
                    << cmdObj,
                state);

        // We should not have allowed an empty 'history'.
        invariant(state->hasNextCursorResponse());
        auto cmdResponsePair = state->peekNextCommandResponsePair();
        assertGivenCommandMatchesExpectedCommand(cmdObj, cmdResponsePair.expectedCommand);


        // If this command returns multiple cursors, we need to claim a state for each one.
        // Note that this only uses one response even though it prepares two cursor states.
        if (cmdResponsePair.response.hasField("cursors")) {
            BSONElement cursorsArrayElem = cmdResponsePair.response.getField("cursors");
            uassert(6253508,
                    "Cursors field in response must be an array",
                    cursorsArrayElem.type() == BSONType::array);
            auto cursorsArray = cursorsArrayElem.Array();
            uassert(
                6253509, "Cursors field must have exactly two cursors", cursorsArray.size() == 2);
            CursorState* secondState = stateGuard->doOrderCheck()
                ? stateGuard->claimAvailableState()
                : stateGuard->claimStateForCommand(placeholderCmd);

            uassert(
                6253510,
                str::stream() << "Could not return multiple cursor states as there are no "
                                 "remaining unclaimed mock cursor states. Attempted response was "
                              << cmdResponsePair.response,
                secondState);
            // Pop the response from the second state if it is a placeholder.
            auto secondResponsePair = secondState->peekNextCommandResponsePair();
            if (mongotmock::checkGivenCommandMatchesExpectedCommand(
                    secondResponsePair.expectedCommand, placeholderCmd)) {
                secondState->popNextCommandResponsePair();
            }
        }

        // Return the queued response.
        result->appendElements(cmdResponsePair.response);

        // Pop the first response.
        state->popNextCommandResponsePair();
    }
};

class MongotMockSearch final : public MongotMockCursorCommand {
public:
    MongotMockSearch() : MongotMockCursorCommand("search") {}
};
MONGO_REGISTER_COMMAND(MongotMockSearch).forShard();

// A command that generates a merging pipeline from a search query.
class MongotMockPlanShardedSearchCommand final : public MongotMockCursorCommand {
public:
    MongotMockPlanShardedSearchCommand() : MongotMockCursorCommand("planShardedSearch") {}
};
MONGO_REGISTER_COMMAND(MongotMockPlanShardedSearchCommand).forShard();


// A command that responds to a vector search query.

class MongotMockVectorSearchCommand final : public MongotMockCursorCommand {
public:
    MongotMockVectorSearchCommand() : MongotMockCursorCommand("vectorSearch") {}
};
MONGO_REGISTER_COMMAND(MongotMockVectorSearchCommand).forShard();


class MongotMockGetMore final : public MongotMockBaseCmd {
public:
    MongotMockGetMore() : MongotMockBaseCmd("getMore") {}

    void processCommand(OperationContext* opCtx,
                        const DatabaseName&,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result) const final {
        // Because the cursorOptions field (which may have been added during the sending of the
        // getMore) is not part of the GetMoreCommandRequest spec, parsing would fail because it is
        // an unknown field. Therefore, we remove that field (if it exists) from the command object
        // we actually parse.
        auto cmdObjStrippedOfCursorOptions = cmdObj.removeField(mongot_cursor::kCursorOptionsField);
        auto cmd = GetMoreCommandRequest::parse(cmdObjStrippedOfCursorOptions,
                                                IDLParserContext{"getMore"});
        const auto cursorId = cmd.getCommandParameter();
        MongotMockStateGuard stateGuard = getMongotMockState(opCtx->getServiceContext());

        CursorState* cursorState = stateGuard->getCursorState(cursorId);
        uassert(31089,
                str::stream() << "Could not find cursor state associated with cursor id "
                              << cursorId,
                cursorState);
        uassert(31087,
                str::stream() << "Cursor for cursor id " << cursorId << " has no queued history",
                cursorState->hasNextCursorResponse());
        uassert(31088,
                str::stream() << "Cannot run getMore on cursor id " << cursorId
                              << " without having run search",
                cursorState->claimed());

        auto cmdResponsePair = cursorState->peekNextCommandResponsePair();
        // Note that killCursors always does order checking to prevent killing cursors if the mock
        // is expecting more commands for that cursor.
        assertGivenCommandMatchesExpectedCommand(cmdObj, cmdResponsePair.expectedCommand);
        result->appendElements(cmdResponsePair.response);

        cursorState->popNextCommandResponsePair();
    }
};
MONGO_REGISTER_COMMAND(MongotMockGetMore).forShard();


class MongotMockKillCursors final : public MongotMockBaseCmd {
public:
    MongotMockKillCursors() : MongotMockBaseCmd("killCursors") {}

    void processCommand(OperationContext* opCtx,
                        const DatabaseName&,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result) const final {
        auto request = KillCursorsCommandRequest::parse(cmdObj, IDLParserContext("killCursors"));

        const auto& cursorList = request.getCursorIds();
        MongotMockStateGuard stateGuard = getMongotMockState(opCtx->getServiceContext());

        uassert(31090,
                "Mock mongot supports killCursors of only one cursor at a time",
                cursorList.size() == 1);

        const auto cursorId = cursorList.front();

        CursorState* cursorState = stateGuard->getCursorState(cursorId);
        uassert(31092,
                str::stream() << "Could not find cursor state associated with cursor id "
                              << cursorId,
                cursorState);
        uassert(31093,
                str::stream() << "Cannot run killCursors on cursor id " << cursorId
                              << " without having run search",
                cursorState->claimed());

        // There appear to be situations in which mongod sends a killCursors command to mongot even
        // after recieving a response with cursorID 0, indicating the end of a stream. While this is
        // unnecessary, there isn't anything in the killCursors spec that prevents this. As such, we
        // should gracefully handle this case.
        if (!cursorState->hasNextCursorResponse()) {
            KillCursorsCommandReply rep;
            rep.setCursorsKilled({});
            rep.setCursorsNotFound(cursorList);
            rep.setCursorsAlive({});
            rep.setCursorsUnknown({});
            result->appendElements(rep.toBSON());
            return;
        }

        auto cmdResponsePair = cursorState->peekNextCommandResponsePair();
        assertGivenCommandMatchesExpectedCommand(cmdObj, cmdResponsePair.expectedCommand);
        result->appendElements(cmdResponsePair.response);

        cursorState->popNextCommandResponsePair();
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "Should not have any responses left after killCursors",
                !cursorState->hasNextCursorResponse());
    }
};
MONGO_REGISTER_COMMAND(MongotMockKillCursors).forShard();

/*
 * If a command needs two cursor states, this command claims a state without needing an additional
 * history.
 */
class MongotMockAllowMultiCursorResponse final : public MongotMockBaseCmd {
public:
    MongotMockAllowMultiCursorResponse() : MongotMockBaseCmd("allowMultiCursorResponse") {}

    void processCommand(OperationContext* opCtx,
                        const DatabaseName&,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result) const final {

        MongotMockStateGuard stateGuard = getMongotMockState(opCtx->getServiceContext());

        uassert(ErrorCodes::InvalidOptions,
                "cursorId should be type NumberLong",
                cmdObj["cursorId"].type() == BSONType::numberLong);
        const CursorId id = cmdObj["cursorId"].Long();
        uassert(ErrorCodes::InvalidOptions, "cursorId may not equal 0", id != 0);

        std::deque<mongotmock::MockedResponse> commandResponsePairs;

        uassert(ErrorCodes::InvalidOptions,
                "allowMultiCursorResponse should not have 'history'",
                !cmdObj.hasField("history"));

        // Use an empty response pair, it will be ignored as the response was specified by the
        // original setMockResponse.
        commandResponsePairs.push_back({placeholderCmd, placeholderResponse});

        stateGuard->setStateForId(id,
                                  std::make_unique<CursorState>(std::move(commandResponsePairs)));
    }
};
MONGO_REGISTER_COMMAND(MongotMockAllowMultiCursorResponse).forShard();

/**
 * For sharding tests we may wind up in a situation where order of responses from shards
 * is not deterministic. Calling this function disables/enables order checking for the
 * lifetime of the mock.
 */
class MongotMockSetOrderCheck final : public MongotMockBaseCmd {
public:
    MongotMockSetOrderCheck() : MongotMockBaseCmd("setOrderCheck") {}
    void processCommand(OperationContext* opCtx,
                        const DatabaseName&,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result) const final {

        MongotMockStateGuard stateGuard = getMongotMockState(opCtx->getServiceContext());
        auto orderField = cmdObj.getField("setOrderCheck");
        uassert(ErrorCodes::InvalidOptions,
                "setOrderCheck must be a single boolean field",
                orderField && orderField.isBoolean());

        stateGuard->setOrderCheck(orderField.boolean());
    }
};
MONGO_REGISTER_COMMAND(MongotMockSetOrderCheck).forShard();

class MongotMockSetMockResponse final : public MongotMockBaseCmd {
public:
    MongotMockSetMockResponse() : MongotMockBaseCmd("setMockResponses") {}

    void processCommand(OperationContext* opCtx,
                        const DatabaseName&,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result) const final {
        MongotMockStateGuard stateGuard = getMongotMockState(opCtx->getServiceContext());

        uassert(ErrorCodes::InvalidOptions,
                "cursorId should be type NumberLong",
                cmdObj["cursorId"].type() == BSONType::numberLong);
        const CursorId id = cmdObj["cursorId"].Long();
        uassert(ErrorCodes::InvalidOptions, "cursorId may not equal 0", id != 0);

        uassert(ErrorCodes::InvalidOptions,
                "'history' should be of type Array",
                cmdObj["history"].type() == BSONType::array);

        std::deque<mongotmock::MockedResponse> mockedResponses;

        for (auto&& cmdResponsePair : cmdObj["history"].embeddedObject()) {
            uassert(ErrorCodes::InvalidOptions,
                    "Each element of 'history' should be an object",
                    cmdResponsePair.type() == BSONType::object);
            uassert(ErrorCodes::InvalidOptions,
                    "Each element of 'history' should have an 'expectedCommand' "
                    "field of type object",
                    cmdResponsePair["expectedCommand"].type() == BSONType::object);
            uassert(ErrorCodes::InvalidOptions,
                    "Each element of 'history' should have a 'response' field of "
                    "type object",
                    cmdResponsePair["response"].type() == BSONType::object);
            if (cmdResponsePair.Obj().hasField("maybeUnused")) {
                uassert(ErrorCodes::InvalidOptions,
                        "The 'maybeUnused' field must be a boolean",
                        cmdResponsePair["maybeUnused"].type() == BSONType::boolean);
            }

            mockedResponses.push_back({
                .expectedCommand = cmdResponsePair["expectedCommand"].embeddedObject().getOwned(),
                .response = cmdResponsePair["response"].embeddedObject().getOwned(),
                .maybeUnused = cmdResponsePair.Obj().hasField("maybeUnused")
                    ? cmdResponsePair["maybeUnused"].Bool()
                    : false,
            });
        }
        uassert(
            ErrorCodes::InvalidOptions, "'history' should not be empty", !mockedResponses.empty());

        stateGuard->setStateForId(id, std::make_unique<CursorState>(std::move(mockedResponses)));
    }
};
MONGO_REGISTER_COMMAND(MongotMockSetMockResponse).forShard();

class MongotMockCloseConnectionOnNextRequests final : public MongotMockBaseCmd {
public:
    MongotMockCloseConnectionOnNextRequests()
        : MongotMockBaseCmd("closeConnectionOnNextRequests") {}
    void processCommand(OperationContext* opCtx,
                        const DatabaseName&,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result) const final {
        MongotMockStateGuard stateGuard = getMongotMockState(opCtx->getServiceContext());
        stateGuard->closeConnectionsToSubsequentCursorCommands(
            cmdObj["closeConnectionOnNextRequests"].Int());
    }
};
MONGO_REGISTER_COMMAND(MongotMockCloseConnectionOnNextRequests).forShard();

/**
 * Command to check if there are any remaining queued responses.
 */
class MongotMockGetQueuedResponses final : public MongotMockBaseCmd {
public:
    MongotMockGetQueuedResponses() : MongotMockBaseCmd("getQueuedResponses") {}

    void processCommand(OperationContext* opCtx,
                        const DatabaseName&,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result) const final {

        MongotMockStateGuard stateGuard = getMongotMockState(opCtx->getServiceContext());

        size_t remainingQueuedResponses = 0;
        const auto& cursorMap = stateGuard->getCursorMap();
        for (auto&& cursorIdCursorStatePair : cursorMap) {
            BSONArrayBuilder arrBuilder(
                result->subobjStart(str::stream() << "cursorID " << cursorIdCursorStatePair.first));

            auto& remainingResponses = cursorIdCursorStatePair.second->getRemainingResponses();
            for (auto&& remainingResponse : remainingResponses) {
                ++remainingQueuedResponses;

                BSONObjBuilder objBuilder(arrBuilder.subobjStart());
                objBuilder.append("expectedCommand", remainingResponse.expectedCommand);
                objBuilder.append("response", remainingResponse.response);
                objBuilder.append("maybeUnused", remainingResponse.maybeUnused);
                objBuilder.doneFast();
            }

            arrBuilder.doneFast();
        }

        result->append("numRemainingResponses", static_cast<int>(remainingQueuedResponses));
    }
};
MONGO_REGISTER_COMMAND(MongotMockGetQueuedResponses).forShard();

class MongotMockClearQueuedResponses final : public MongotMockBaseCmd {
public:
    MongotMockClearQueuedResponses() : MongotMockBaseCmd("clearQueuedResponses") {}
    void processCommand(OperationContext* opCtx,
                        const DatabaseName&,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result) const final {
        auto clearField = cmdObj.getField("clearQueuedResponses");
        uassert(ErrorCodes::InvalidOptions,
                "clearQueuedResponses must be an empty object",
                clearField && clearField.Obj().isEmpty());
        MongotMockStateGuard stateGuard = getMongotMockState(opCtx->getServiceContext());
        stateGuard->clearStates();
        result->append("numRemainingResponses", 0);
    }
};
MONGO_REGISTER_COMMAND(MongotMockClearQueuedResponses).forShard();

/**
 * Sets the command response returned by the 'manageSearchIndex' command mock for a single call.
 * Must be called prior to each 'manageSearchIndex' command to set a mock response because the
 * response is good only for a single request. Is not idempotent, expects the 'manageSearchIndex'
 * command to be called before this command can be called again.
 */
class MongotMockSetManageSearchIndexResponse final : public MongotMockBaseCmd {
public:
    MongotMockSetManageSearchIndexResponse() : MongotMockBaseCmd("setManageSearchIndexResponse") {}

    void processCommand(OperationContext* opCtx,
                        const DatabaseName&,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result) const final {
        auto elem = cmdObj.getField("manageSearchIndexResponse");
        if (!elem.eoo()) {
            MongotMockStateGuard stateGuard = getMongotMockState(opCtx->getServiceContext());
            uassert(ErrorCodes::InvalidOptions,
                    "A response has already been set. 'manageSearchIndex' must be called before "
                    "setting a new response",
                    stateGuard->getMockManageSearchIndexResponse().isEmpty());
            stateGuard->setMockManageSearchIndexResponse(elem.Obj());
        }
    }
};
MONGO_REGISTER_COMMAND(MongotMockSetManageSearchIndexResponse).forShard();

/**
 * This is the search index management endpoint mock. Any command started with 'manageSearchIndex'
 * will receive the response currently set on the MongotMockState.
 *
 * The command requires that _something_ be set on the MongotMockState by the
 * 'setManageSearchIndexResponse' command prior to calling _this_ command. And whenever a mock
 * response is set, it will only be good for _one_ 'manageSearchIndex' call.
 */
class MongotMockManageSearchIndex final : public MongotMockBaseCmd {
public:
    MongotMockManageSearchIndex() : MongotMockBaseCmd("manageSearchIndex") {}

    void processCommand(OperationContext* opCtx,
                        const DatabaseName&,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result) const final {
        {
            // Verify that the command request is valid.
            IDLParserContext ctx("ManageSearchIndexRequest Parser");
            ManageSearchIndexRequest request = ManageSearchIndexRequest::parse(cmdObj, ctx);
        }

        MongotMockStateGuard stateGuard = getMongotMockState(opCtx->getServiceContext());
        auto managementResponse = stateGuard->getMockManageSearchIndexResponse();

        uassert(ErrorCodes::InvalidOptions,
                "No response has been set by 'setManageSearchIndexResponse'",
                !managementResponse.isEmpty());

        for (auto& elem : managementResponse) {
            result->append(elem);
        }

        // Clear the mock response: it is good for only a single call because only one call should
        // be made per mongod command.
        stateGuard->setMockManageSearchIndexResponse(BSONObj());
    }
};
MONGO_REGISTER_COMMAND(MongotMockManageSearchIndex).forShard();

}  // namespace
}  // namespace mongo
