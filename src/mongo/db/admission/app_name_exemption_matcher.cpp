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

#include "mongo/db/admission/app_name_exemption_matcher.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/app_name_exemption_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/metadata/client_metadata.h"


namespace mongo {

namespace admission {

AppNameExemptionMatcher::AppNameExemptionMatcher(VersionedValue<ExemptionList>& exemptions)
    : _exemptions(exemptions) {}

bool AppNameExemptionMatcher::isExempted(Client* client) {
    if (MONGO_unlikely(!_exempted.has_value() || !_exemptions.isCurrent(_snapshot))) {
        _exempted = _matchesExemptionList(client);
    }
    return *_exempted;
}

bool AppNameExemptionMatcher::isExemptionListSnapshotCurrent() const {
    return _exemptions.isCurrent(_snapshot);
}

bool AppNameExemptionMatcher::_matchesExemptionList(Client* client) {
    const auto* clientMetadata = ClientMetadata::get(client);
    if (!clientMetadata) {
        return false;
    }

    // Don't refresh the snapshot until we've seen client metadata, to ensure that we don't cache an
    // outdated exempted status based on non-existent client metadata.
    _exemptions.refreshSnapshot(_snapshot);
    if (!_snapshot) {
        return false;
    }

    for (const auto& exemption : *_snapshot) {
        if (clientMetadata->getDriverName().starts_with(exemption) ||
            clientMetadata->getApplicationName().starts_with(exemption)) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<std::vector<std::string>> parseAppNameExemptionList(const BSONObj& obj) {
    IDLParserContext ctx("ApplicationExemptionListParameters");
    auto params = ApplicationExemptionListParameters::parse(obj, ctx);

    auto result = std::make_shared<std::vector<std::string>>();
    for (const auto& app : params.getAppNames()) {
        result->emplace_back(std::string{app});
    }
    return result;
}

void appendAppNameExemptionList(const VersionedValue<std::vector<std::string>>::Snapshot& snapshot,
                                BSONObjBuilder* bob,
                                StringData name) {
    if (snapshot) {
        BSONObjBuilder subObj(bob->subobjStart(name));
        BSONArrayBuilder bb(bob->subarrayStart("appNames"));
        for (const auto& appName : *snapshot) {
            bb << appName;
        }
    } else {
        *bob << name << BSONNULL;
    }
}

}  // namespace admission

}  // namespace mongo
