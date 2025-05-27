/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/db/feature_flag_server_parameter.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/server_parameter.h"
#include "mongo/util/assert_util.h"

#include <boost/optional/optional.hpp>

namespace mongo {

FeatureFlagServerParameter::FeatureFlagServerParameter(StringData name, FeatureFlag* flag)
    : ServerParameter(name,
                      flag->allowRuntimeToggle() ? ServerParameterType::kStartupAndRuntime
                                                 : ServerParameterType::kStartupOnly),
      _flag(flag) {}

void FeatureFlagServerParameter::append(OperationContext* opCtx,
                                        BSONObjBuilder* b,
                                        StringData name,
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
                                                           StringData name,
                                                           const boost::optional<TenantId>&) {
    b->append(name, _flag->getForServerParameter());
}

Status FeatureFlagServerParameter::set(const BSONElement& newValueElement,
                                       const boost::optional<TenantId>&) {
    bool newValue;

    if (auto status = newValueElement.tryCoerce(&newValue); !status.isOK()) {
        return {status.code(),
                str::stream() << "Failed setting " << name() << ": " << status.reason()};
    }

    try {
        _flag->setForServerParameter(newValue);
    } catch (DBException& e) {
        return e.toStatus();
    }

    return Status::OK();
}

Status FeatureFlagServerParameter::setFromString(StringData str, const boost::optional<TenantId>&) {
    auto swNewValue = idl_server_parameter_detail::coerceFromString<bool>(str);
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
