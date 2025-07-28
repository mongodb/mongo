/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/historical_placement_fetcher.h"

#include <deque>
#include <type_traits>
#include <utility>
#include <variant>

namespace mongo {

class HistoricalPlacementFetcherMock : public HistoricalPlacementFetcher {
public:
    using TimestampOrNss = std::variant<Timestamp, boost::optional<NamespaceString>>;
    using Response = std::pair<TimestampOrNss, HistoricalPlacement>;

    /**
     * Queue the provided responses in the mock.
     */
    template <typename T>
    requires(std::is_same_v<typename T::value_type, Response>)
    void bufferResponses(const T& responses) {
        std::copy(responses.begin(), responses.end(), std::back_inserter(_responses));
    }

    bool empty() const {
        return _responses.empty();
    }

    /**
     * Check that the provided cluster time or NamespaceString is equal to the one provided in the
     * buffered response and then hand out the buffered HistoricalPlacement for it.
     */
    HistoricalPlacement fetch(OperationContext* opCtx,
                              const boost::optional<NamespaceString>& nss,
                              Timestamp atClusterTime) override {
        auto response = popResponse(nss, atClusterTime);
        return response.second;
    }

private:
    Response popResponse(const boost::optional<NamespaceString>& nss, Timestamp atClusterTime) {
        tassert(10718900, "queue should not be empty", !empty());

        auto response = _responses.front();
        if (std::holds_alternative<Timestamp>(response.first)) {
            tassert(10718901,
                    "buffered response should contain a timestamp",
                    std::holds_alternative<Timestamp>(response.first));
            tassert(10718902,
                    "cluster time should be equal to expected cluster time",
                    std::get<Timestamp>(response.first) == atClusterTime);
        } else {
            tassert(10718903,
                    "buffered response should contain a NamespaceString",
                    std::holds_alternative<boost::optional<NamespaceString>>(response.first));
            tassert(10718904,
                    "NamespaceString should be equal to the expected nss",
                    std::get<boost::optional<NamespaceString>>(response.first) == nss);
        }
        _responses.pop_front();
        return response;
    }

    std::deque<Response> _responses;
};

}  // namespace mongo
