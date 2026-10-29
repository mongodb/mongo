// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/versioned_value.h"

#include <string>
#include <string_view>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {

class Client;
class OperationContext;

namespace admission {

/**
 * Matches client driver/application names against a configurable exemption list.
 *
 * Used by admission control features (execution control deprioritization, ingress rate limiting)
 * to exempt certain clients.
 *
 * A client is considered exempted if any string in the exemption list is a prefix of the client's
 * application or driver name.
 */
class AppNameExemptionMatcher {
public:
    using ExemptionList = std::vector<std::string>;
    using Snapshot = VersionedValue<ExemptionList>::Snapshot;

    explicit AppNameExemptionMatcher(VersionedValue<ExemptionList>& exemptions);

    bool isExempted(Client* client);

    bool isExemptionListSnapshotCurrent() const;

private:
    bool _matchesExemptionList(Client* client);

    VersionedValue<ExemptionList>& _exemptions;
    Snapshot _snapshot;
    boost::optional<bool> _exempted;
};

std::shared_ptr<std::vector<std::string>> parseAppNameExemptionList(const BSONObj& obj);

void appendAppNameExemptionList(const VersionedValue<std::vector<std::string>>::Snapshot& snapshot,
                                BSONObjBuilder* bob,
                                std::string_view name);

}  // namespace admission

}  // namespace mongo
