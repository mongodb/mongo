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

#pragma once

#include <string>

#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/server_options.h"
#include "mongo/idl/server_parameter.h"
#include "mongo/util/version/releases.h"

namespace mongo {

/**
 * FeatureFlag contains information about whether a feature flag is enabled and what version it was
 * released.
 *
 * FeatureFlag represents the state of a feature flag and whether it is associated with a particular
 * version. It is not implicitly convertible to bool to force all call sites to make a decision
 * about what check to use.
 *
 * It is only set at startup.
 */
class FeatureFlag {
    friend class FeatureFlagServerParameter;

public:
    FeatureFlag(bool enabled, StringData versionString);

    /**
     * Returns true if the flag is set to true and enabled for this FCV version.
     */
    bool isEnabled(const ServerGlobalParams::FeatureCompatibility& fcv) const;

    /**
     * Returns true if this flag is enabled regardless of the current FCV version.
     *
     * isEnabled() is prefered over this function since it will prevent upgrade/downgrade issues.
     */
    bool isEnabledAndIgnoreFCV() const;

    /**
     * Returns true if the flag is set to true and enabled on the target FCV version.
     *
     * This function is used in the 'setFeatureCompatibilityVersion' command where the in-memory FCV
     * is in flux.
     */
    bool isEnabledOnVersion(multiversion::FeatureCompatibilityVersion targetFCV) const;

    /**
     * Return the version associated with this feature flag.
     *
     * Throws if feature is not enabled.
     */
    multiversion::FeatureCompatibilityVersion getVersion() const;

private:
    void set(bool enabled);

private:
    bool _enabled;
    multiversion::FeatureCompatibilityVersion _version;
};

/**
 * Specialization of ServerParameter for FeatureFlags used by IDL generator.
 */
class FeatureFlagServerParameter : public ServerParameter {
public:
    FeatureFlagServerParameter(StringData name, FeatureFlag& storage);

    /**
     * Encode the setting into BSON object.
     *
     * Typically invoked by {getParameter:...} to produce a dictionary
     * of ServerParameter settings.
     */
    void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name) final;

    /**
     * Encode the feature flag value into a BSON object, discarding the version.
     */
    void appendSupportingRoundtrip(OperationContext* opCtx,
                                   BSONObjBuilder& b,
                                   const std::string& name) override;

    /**
     * Update the underlying value using a BSONElement
     *
     * Allows setting non-basic values (e.g. vector<string>)
     * via the {setParameter: ...} call.
     */
    Status set(const BSONElement& newValueElement) final;

    /**
     * Update the underlying value from a string.
     *
     * Typically invoked from commandline --setParameter usage.
     */
    Status setFromString(const std::string& str) final;

private:
    FeatureFlag& _storage;
};

inline FeatureFlagServerParameter* makeFeatureFlagServerParameter(StringData name,
                                                                  FeatureFlag& storage) {
    auto p = std::make_unique<FeatureFlagServerParameter>(name, storage);
    registerServerParameter(&*p);
    return p.release();
}

}  // namespace mongo
