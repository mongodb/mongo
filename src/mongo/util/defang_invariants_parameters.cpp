// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/util/defang_invariants_parameters_gen.h"
#include "mongo/util/invariant_action_hook.h"
#include "mongo/util/string_map.h"
#include "mongo/util/version.h"

#include <algorithm>
#include <iterator>

#include <fmt/core.h>

namespace mongo {

namespace {
// Cannot use synchronized value because we must use RWMutex
RWMutex defangedInvariantsMutex;
BSONObj defangedInvariantsAllVersions;
StringSet defangedInvariants;

bool lookupInvariantAbortsImpl(std::string_view invariantID) {
    auto _ = std::shared_lock{defangedInvariantsMutex};
    return !defangedInvariants.contains(invariantID);
}

Status setDefangedInvariants(const BSONObj& obj) {
    auto versionElement = obj.getField(VersionInfoInterface::instance().version());

    if (versionElement.eoo()) {
        auto _ = std::unique_lock{defangedInvariantsMutex};
        defangedInvariants.clear();
        defangedInvariantsAllVersions = obj.getOwned();
        return Status::OK();
    }

    if (versionElement.type() != BSONType::array) {
        return Status(ErrorCodes::TypeMismatch,
                      fmt::format("Expected the list of defanged invariants to be an array {}",
                                  versionElement.toString()));
    }

    auto defangedInvariantObj = versionElement.Array();

    if (!std::all_of(defangedInvariantObj.begin(),
                     defangedInvariantObj.end(),
                     [](const BSONElement& elem) { return elem.type() == BSONType::string; })) {
        return Status(
            ErrorCodes::TypeMismatch,
            fmt::format(
                "One or more invariants in the list of defanged invariants are not strings {}",
                versionElement.toString()));
    }

    StringSet newDefangedInvariants;
    newDefangedInvariants.reserve(defangedInvariantObj.size());
    std::transform(defangedInvariantObj.begin(),
                   defangedInvariantObj.end(),
                   std::inserter(newDefangedInvariants, newDefangedInvariants.begin()),
                   [](const BSONElement& elem) { return elem.String(); });

    auto _ = std::unique_lock{defangedInvariantsMutex};
    defangedInvariants = std::move(newDefangedInvariants);
    defangedInvariantsAllVersions = obj.getOwned();
    return Status::OK();
}
}  // namespace

void DefangedInvariantsServerParameter::append(OperationContext*,
                                               BSONObjBuilder* b,
                                               std::string_view name,
                                               const boost::optional<TenantId>&) {
    auto _ = std::shared_lock{defangedInvariantsMutex};
    b->append(name, defangedInvariantsAllVersions);
}

Status DefangedInvariantsServerParameter::set(const BSONElement& newValueElement,
                                              const boost::optional<TenantId>&) {
    if (!newValueElement.isABSONObj()) {
        return Status(
            ErrorCodes::TypeMismatch,
            fmt::format(
                "defangedInvariants parameter is expected to be an object. Actual: {} value: {}",
                typeName(newValueElement.type()),
                newValueElement.toString()));
    }
    return setDefangedInvariants(newValueElement.Obj());
}

Status DefangedInvariantsServerParameter::setFromString(std::string_view str,
                                                        const boost::optional<TenantId>&) try {
    return setDefangedInvariants(fromjson(str));
} catch (const DBException& ex) {
    return ex.toStatus();
}

MONGO_INITIALIZER(RegisterInvariantActionLookup)(InitializerContext*) {
    setLookupInvariantAbortsFn(lookupInvariantAbortsImpl);
}

}  // namespace mongo
