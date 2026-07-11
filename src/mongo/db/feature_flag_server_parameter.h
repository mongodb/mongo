// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"

#include <string_view>
#include <variant>

#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Specialization of ServerParameter for FeatureFlags used by IDL generator.
 */
class FeatureFlagServerParameter : public ServerParameter {
public:
    FeatureFlagServerParameter(std::string_view name, FeatureFlag* flag);

    /**
     * Encode the setting into BSON object.
     *
     * Typically invoked by {getParameter:...} to produce a dictionary
     * of ServerParameter settings.
     */
    void append(OperationContext* opCtx,
                BSONObjBuilder* b,
                std::string_view name,
                const boost::optional<TenantId>&) final;

    void appendDetails(OperationContext* opCtx,
                       BSONObjBuilder* detailsBuilder,
                       const boost::optional<TenantId>& tenantId) final;

    /**
     * Encode the feature flag value into a BSON object, discarding the version.
     */
    void appendSupportingRoundtrip(OperationContext* opCtx,
                                   BSONObjBuilder* b,
                                   std::string_view name,
                                   const boost::optional<TenantId>&) override;

    /**
     * Update the underlying value using a BSONElement
     *
     * Allows setting non-basic values (e.g. vector<string>)
     * via the {setParameter: ...} call.
     */
    Status set(const BSONElement& newValueElement, const boost::optional<TenantId>&) final;

    /**
     * Update the underlying value from a string.
     *
     * Typically invoked from commandline --setParameter usage.
     */
    Status setFromString(std::string_view str, const boost::optional<TenantId>&) final;

    bool isForIncrementalFeatureRollout() const final;

    void onRegistrationWithProcessGlobalParameterList() override;

private:
    FeatureFlag* _flag;
};

}  // namespace mongo
