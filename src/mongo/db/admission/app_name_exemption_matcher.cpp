// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/app_name_exemption_matcher.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/app_name_exemption_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/metadata/client_metadata.h"

#include <string_view>


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
                                std::string_view name) {
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
