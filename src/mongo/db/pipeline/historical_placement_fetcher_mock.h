// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/historical_placement_fetcher.h"
#include "mongo/util/modules.h"

#include <deque>
#include <type_traits>
#include <utility>
#include <variant>

namespace mongo {

class [[MONGO_MOD_UNFORTUNATELY_OPEN]] HistoricalPlacementFetcherMock
    : public HistoricalPlacementFetcher {
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
                              Timestamp atClusterTime,
                              bool checkIfPointInTimeIsInFuture,
                              bool ignoreRemovedShards) override {
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
