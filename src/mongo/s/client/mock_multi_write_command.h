/**
 *    Copyright (C) 2013 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <deque>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/s/client/multi_command_dispatch.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

/**
 * A ConnectionString endpoint registered with some kind of error, to simulate returning when
 * the endpoint is used.
 */
struct MockWriteResult {
    MockWriteResult(const ConnectionString& endpoint, const WriteErrorDetail& error)
        : endpoint(endpoint) {
        WriteErrorDetail* errorCopy = new WriteErrorDetail;
        error.cloneTo(errorCopy);
        errorCopy->setIndex(0);
        response.setOk(true);
        response.setN(0);
        response.addToErrDetails(errorCopy);
    }

    MockWriteResult(const ConnectionString& endpoint, const WriteErrorDetail& error, int copies)
        : endpoint(endpoint) {
        response.setOk(true);
        response.setN(0);

        for (int i = 0; i < copies; ++i) {
            WriteErrorDetail* errorCopy = new WriteErrorDetail;
            error.cloneTo(errorCopy);
            errorCopy->setIndex(i);
            response.addToErrDetails(errorCopy);
        }
    }


    MockWriteResult(const ConnectionString& endpoint, const BatchedCommandResponse& response)
        : endpoint(endpoint) {
        response.cloneTo(&this->response);
    }

    const ConnectionString endpoint;
    BatchedCommandResponse response;
};

/**
 * Implementation of the MultiCommandDispatch interface which allows registering a number of
 * endpoints on which errors are returned.  Note that *only* BatchedCommandResponses are
 * supported here.
 *
 * The first matching MockEndpoint for a request in the MockEndpoint* vector is used for one
 * request, then removed.  This allows simulating retryable errors where a second request
 * succeeds or has a different error reported.
 *
 * If an endpoint isn't registered with a MockEndpoint, just returns BatchedCommandResponses
 * with ok : true.
 */
class MockMultiWriteCommand : public MultiCommandDispatch {
public:
    void init(const std::vector<MockWriteResult*> mockEndpoints) {
        ASSERT(!mockEndpoints.empty());
        _mockEndpoints.mutableVector().insert(
            _mockEndpoints.mutableVector().end(), mockEndpoints.begin(), mockEndpoints.end());
    }

    void addCommand(const ConnectionString& endpoint,
                    StringData dbName,
                    const BSONObj& request) override {
        _pending.push_back(endpoint);
    }

    void sendAll() override {
        // No-op
    }

    int numPending() const override {
        return static_cast<int>(_pending.size());
    }

    /**
     * Returns an error response if the next pending endpoint returned has a corresponding
     * MockEndpoint.
     */
    Status recvAny(ConnectionString* endpoint, BSONSerializable* response) override {
        BatchedCommandResponse* batchResponse =  //
            static_cast<BatchedCommandResponse*>(response);

        *endpoint = _pending.front();
        MockWriteResult* mockResponse = releaseByHost(_pending.front());
        _pending.pop_front();

        if (NULL == mockResponse) {
            batchResponse->setOk(true);
            batchResponse->setN(0);  // TODO: Make this accurate
        } else {
            mockResponse->response.cloneTo(batchResponse);
            delete mockResponse;
        }

        ASSERT(batchResponse->isValid(NULL));
        return Status::OK();
    }

    const std::vector<MockWriteResult*>& getEndpoints() const {
        return _mockEndpoints.vector();
    }

private:
    // Find a MockEndpoint* by host, and release it so we don't see it again
    MockWriteResult* releaseByHost(const ConnectionString& endpoint) {
        std::vector<MockWriteResult*>& endpoints = _mockEndpoints.mutableVector();

        for (std::vector<MockWriteResult*>::iterator it = endpoints.begin(); it != endpoints.end();
             ++it) {
            MockWriteResult* storedEndpoint = *it;
            if (storedEndpoint->endpoint.toString().compare(endpoint.toString()) == 0) {
                endpoints.erase(it);
                return storedEndpoint;
            }
        }

        return NULL;
    }

    // Manually-stored ranges
    OwnedPointerVector<MockWriteResult> _mockEndpoints;

    std::deque<ConnectionString> _pending;
};

}  // namespace mongo
