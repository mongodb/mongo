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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"

#include <deque>
#include <map>

namespace mongo {
namespace mongotmock {

// Do a "loose" check that every field in 'expectedCmd' is in 'givenCmd'. Checks that all fields in
// 'givenCmd' are in 'expectedCmd' expect for fields in 'ignoredFields'.
bool checkGivenCommandMatchesExpectedCommand(const BSONObj& givenCmd, const BSONObj& expectedCmd);

struct MockedResponse {
    BSONObj expectedCommand;
    BSONObj response;
    // When true, indicates that this mocked response is not guarenteed to be used as part of a
    // test. For example, mongod does not always request data from metadata cursors if $$SEARCH_META
    // is not referenced.
    bool maybeUnused;
};

class CursorState final {
public:
    CursorState(std::deque<MockedResponse> cmdResponsePairs)
        : _remainingResponses(std::move(cmdResponsePairs)) {}

    bool claimed() const {
        return _claimed;
    }

    bool hasNextCursorResponse() const {
        return !_remainingResponses.empty();
    }

    MockedResponse peekNextCommandResponsePair() const {
        invariant(hasNextCursorResponse());
        return _remainingResponses.front();
    }

    MockedResponse findCommandResponsePairMatching(const BSONObj& givenCmd) const {
        invariant(hasNextCursorResponse());
        for (const auto& setResponse : _remainingResponses) {
            if (checkGivenCommandMatchesExpectedCommand(givenCmd, setResponse.expectedCommand)) {
                return setResponse;
            }
        }
        uasserted(6750400, str::stream() << "No command matched " << givenCmd);
    }

    void popNextCommandResponsePair() {
        invariant(hasNextCursorResponse());
        _remainingResponses.pop_front();
    }

    const std::deque<MockedResponse>& getRemainingResponses() {
        return _remainingResponses;
    }

private:
    void claim() {
        invariant(!_claimed);
        _claimed = true;
    }

    std::deque<MockedResponse> _remainingResponses;

    // Whether some client is already using/iterating this state.
    bool _claimed = false;

    // MongotMockState is a friend so that it may call claim() on a CursorState.
    friend class MongotMockState;
};

class MongotMockStateGuard;
class MongotMockState final {
public:
    using CursorMap = std::map<CursorId, std::unique_ptr<CursorState>>;

    void setStateForId(CursorId id, std::unique_ptr<CursorState> state) {
        auto it = _cursorStates.find(id);
        if (it == _cursorStates.end()) {
            // No existing state. Just insert and we're done.
            _availableCursorIds.push_back(id);
            _cursorStates.insert(CursorMap::value_type(id, std::move(state)));
        } else {
            if (!it->second->claimed()) {
                // There is an existing unclaimed state. We must remove it from the list of
                // available cursors. Later, we will re-insert it to the end of the list.
                auto it = std::find(_availableCursorIds.begin(), _availableCursorIds.end(), id);
                invariant(it != _availableCursorIds.end());
                _availableCursorIds.erase(it);
            }

            // Now we are guaranteed that there is no entry for this cursor in _availableCursorIds.
            _availableCursorIds.push_back(id);
            _cursorStates.insert_or_assign(id, std::move(state));
        }
    }

    CursorState* claimAvailableState() {
        if (_availableCursorIds.empty()) {
            return nullptr;
        }
        CursorId id = _availableCursorIds.front();
        _availableCursorIds.pop_front();

        auto it = _cursorStates.find(id);
        invariant(it != _cursorStates.end());
        it->second->claim();
        return it->second.get();
    }

    CursorState* claimStateForCommand(const BSONObj& cmd) {
        if (_availableCursorIds.empty()) {
            return nullptr;
        }
        // Find a cursor state that has a response for the command we received. Use the first one we
        // find.
        for (auto it = _availableCursorIds.begin(); it != _availableCursorIds.end(); ++it) {
            auto potentialCommand = _cursorStates[*it]->peekNextCommandResponsePair();
            if (checkGivenCommandMatchesExpectedCommand(cmd, potentialCommand.expectedCommand)) {
                auto cursorID = *it;
                auto mapIt = _cursorStates.find(cursorID);
                invariant(mapIt != _cursorStates.end());
                _availableCursorIds.erase(it);
                mapIt->second->claim();
                return mapIt->second.get();
            }
        }
        uasserted(6750402, str::stream() << "No cursor has a command matching " << cmd);
    }

    const CursorMap& getCursorMap() {
        return _cursorStates;
    }

    void clearStates() {
        _availableCursorIds.clear();
        _cursorStates.clear();
    }

    CursorState* getCursorState(CursorId id) {
        auto it = _cursorStates.find(id);
        if (it == _cursorStates.end()) {
            return nullptr;
        }
        return it->second.get();
    }

    void setMockManageSearchIndexResponse(const BSONObj& bson) {
        _mockManageSearchIndexResponse = bson.getOwned();
    }

    const BSONObj& getMockManageSearchIndexResponse() {
        return _mockManageSearchIndexResponse;
    }
    void setOrderCheck(bool checkOrder) {
        _doOrderChecks = checkOrder;
    }
    auto doOrderCheck() const {
        return _doOrderChecks;
    }
    void closeConnectionsToSubsequentCursorCommands(int n) {
        _nNextRequestsToCloseConnection = n;
    }
    bool shouldCloseConnection() {
        return _nNextRequestsToCloseConnection;
    }
    void consumeCloseConnection() {
        invariant(_nNextRequestsToCloseConnection > 0);
        _nNextRequestsToCloseConnection--;
    }

private:
    // List of unused cursor ids ordered by insertion time (oldest to newest).
    std::list<CursorId> _availableCursorIds;
    CursorMap _cursorStates;

    // Mock response to a manageSearchIndex command request.
    BSONObj _mockManageSearchIndexResponse;

    // Protects access to all members. Should be acquired using a MongotMockStateGuard.
    stdx::mutex _lock;

    friend class MongotMockStateGuard;
    bool _doOrderChecks = true;
    // The mongotmock will respond to the next _nNextRequestsToCloseConnection requests
    // by just closing the connection they were sent on.
    int _nNextRequestsToCloseConnection = 0;
};

class MongotMockStateGuard final {
public:
    MongotMockStateGuard(MongotMockState* s) : lk(s->_lock), state(s) {}

    MongotMockState* operator->() const {
        return state;
    }

private:
    stdx::lock_guard<stdx::mutex> lk;
    MongotMockState* state;
};

/**
 * Provides access to a service context scoped mock state.
 */
MongotMockStateGuard getMongotMockState(ServiceContext* svc);
}  // namespace mongotmock
}  // namespace mongo
