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

#include "mongo/util/versioned_value.h"

#include <string>
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
                                StringData name);

}  // namespace admission

}  // namespace mongo
