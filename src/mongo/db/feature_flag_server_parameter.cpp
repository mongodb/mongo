// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/feature_flag_server_parameter.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/server_parameter.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {

FeatureFlagServerParameter::FeatureFlagServerParameter(std::string_view name, FeatureFlag* flag)
    : ServerParameter(name,
                      flag->allowRuntimeToggle() ? ServerParameterType::kStartupAndRuntime
                                                 : ServerParameterType::kStartupOnly),
      _flag(flag) {}

void FeatureFlagServerParameter::append(OperationContext* opCtx,
                                        BSONObjBuilder* b,
                                        std::string_view name,
                                        const boost::optional<TenantId>&) {
    BSONObjBuilder flagBuilder(b->subobjStart(name));
    _flag->appendFlagValueAndMetadata(flagBuilder);
}

void FeatureFlagServerParameter::appendDetails(OperationContext* opCtx,
                                               BSONObjBuilder* detailsBuilder,
                                               const boost::optional<TenantId>&) {
    _flag->appendFlagDetails(*detailsBuilder);
}

void FeatureFlagServerParameter::appendSupportingRoundtrip(OperationContext* opCtx,
                                                           BSONObjBuilder* b,
                                                           std::string_view name,
                                                           const boost::optional<TenantId>&) {
    b->append(name, _flag->getForServerParameter());
}

Status FeatureFlagServerParameter::set(const BSONElement& newValueElement,
                                       const boost::optional<TenantId>&) {
    try {
        bool newValue = [&]() {
            if (newValueElement.isABSONObj()) {
                auto parameterDocument = newValueElement.embeddedObject();
                uassert(11033800,
                        "Feature flag document must at least be of form {value: <bool>}",
                        parameterDocument.nFields() >= 1);
                auto embeddedValue = newValueElement["value"];
                uassert(11033801,
                        "Feature flag document must have 'value' field",
                        !embeddedValue.eoo());
                return embeddedValue.trueValue();
            } else {
                return newValueElement.trueValue();
            }
        }();

        _flag->setForServerParameter(newValue);
    } catch (DBException& e) {
        return e.toStatus();
    }

    return Status::OK();
}

Status FeatureFlagServerParameter::setFromString(std::string_view str,
                                                 const boost::optional<TenantId>&) {
    auto swNewValue = coerceFromString<bool>(str);
    if (!swNewValue.isOK()) {
        return swNewValue.getStatus();
    }

    try {
        _flag->setForServerParameter(swNewValue.getValue());
    } catch (DBException& e) {
        return e.toStatus();
    }

    return Status::OK();
}

bool FeatureFlagServerParameter::isForIncrementalFeatureRollout() const {
    return _flag->isForIncrementalFeatureRollout();
}

void FeatureFlagServerParameter::onRegistrationWithProcessGlobalParameterList() {
    _flag->registerFlag();
}
}  // namespace mongo
