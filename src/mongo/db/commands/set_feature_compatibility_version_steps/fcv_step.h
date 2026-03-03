/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/util/modules.h"
#include "mongo/util/version/releases.h"

#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * FCVSteps are the steps which need to be executed
 * when FCV is upgraded/downgraded.
 * Using this interface avoids the need to manually hook the various places in
 * SetFeatureCompatibilityVersionCommand where the upgrade/downgrade steps are called.
 *
 * To define an FCV step, a class need to:
 *
 * 1. Inherit from FCVStep
 * 2. Implement the virtual methods in FCVStep (empty implementations are provided where it makes
 * sense)
 * 3. Store a singleton object of the class somewhere (ideally as a ServiceContext decoration).
 * 4. Define a public static `get(ServiceContext*)` function.
 * 5. Define a static FCVStepRegistry::Registerer object to declare the name of the step.
 *
 * Example:
 *
 * #include "mongo/db/commands/set_feature_compatibility_version_steps/fcv_step.h"
 *
 * class FooStep : public FCVStep {
 * public:
 *     static FooStep* get(ServiceContext* serviceContext);
 *
 *     // ...
 *
 * private:
 *
 *     // Mandatory:
 *     void userCollectionsUassertsForUpgrade(OperationContext* opCtx,
 *                                            FCV originalVersion,
 *                                            FCV requestedVersion) {
 *     ...
 *     }
 *
 *     void userCollectionsWorkForUpgrade(OperationContext* opCtx,
 *                                        FCV originalVersion,
 *                                        FCV requestedVersion) {
 *     ...
 *     }
 *
 *
 * namespace {
 *
 * const auto _fooStepDecoration = ServiceContext::declareDecoration<FooStep>();
 *
 * const FCVStepRegistry::Registerer<FooStep> _FooStepRegisterer("FooStep");
 *
 * }  // namespace
 *
 * FooStep* FooStep::get(ServiceContext* serviceContext) {
 *     return &_fooStepDecoration(serviceContext);
 * }
 */

/**
 * Main API implemented by each FCV step
 */
class MONGO_MOD_PRIVATE FCVStep {
public:
    using FCV = multiversion::FeatureCompatibilityVersion;
    virtual void prepareToUpgradeActionsBeforeGlobalLock(OperationContext* opCtx,
                                                         FCV originalVersion,
                                                         FCV requestedVersion) {}

    virtual void userCollectionsUassertsForUpgrade(OperationContext* opCtx,
                                                   FCV originalVersion,
                                                   FCV requestedVersion) {}

    virtual void userCollectionsWorkForUpgrade(OperationContext* opCtx,
                                               FCV originalVersion,
                                               FCV requestedVersion) {}

    virtual void upgradeServerMetadata(OperationContext* opCtx,
                                       FCV originalVersion,
                                       FCV requestedVersion) {}

    virtual void finalizeUpgrade(OperationContext* opCtx, FCV requestedVersion) {}

    virtual void prepareToDowngradeActions(OperationContext* opCtx,
                                           FCV originalVersion,
                                           FCV requestedVersion) {}

    virtual void userCollectionsUassertsForDowngrade(OperationContext* opCtx,
                                                     FCV originalVersion,
                                                     FCV requestedVersion) {}

    virtual void internalServerCleanupForDowngrade(OperationContext* opCtx,
                                                   FCV originalVersion,
                                                   FCV requestedVersion) {}

    virtual void finalizeDowngrade(OperationContext* opCtx, FCV requestedVersion) {}

    /**
     * Returns the name of the step. Used for logging purposes.
     */
    virtual std::string getStepName() const = 0;

    /*
     * Allows a step not to register
     */
    virtual bool shouldRegisterFCVStep() const {
        return true;
    }
};


/**
 * The registry of FCVSteps.
 */
class MONGO_MOD_PUB FCVStepRegistry final : public FCVStep {
    FCVStepRegistry(const FCVStepRegistry&) = delete;
    FCVStepRegistry& operator=(const FCVStepRegistry&) = delete;

public:
    template <class ActualStep>
    class Registerer {
        Registerer(const Registerer&) = delete;
        Registerer& operator=(const Registerer&) = delete;

    public:
        explicit Registerer(std::string name)
            : _registerer(
                  std::move(name),
                  [&](ServiceContext* serviceContext) {
                      if (!_registered) {
                          _registered = ActualStep::get(serviceContext)->shouldRegisterFCVStep();
                      }
                      if (*_registered) {
                          FCVStepRegistry::get(serviceContext)
                              ._registerFeature(ActualStep::get(serviceContext));
                      }
                  },
                  [&](ServiceContext* serviceContext) {
                      if (_registered && *_registered) {
                          FCVStepRegistry::get(serviceContext)
                              ._unregisterFeature(ActualStep::get(serviceContext));
                      }
                  }) {}

    private:
        boost::optional<bool> _registered;
        ServiceContext::ConstructorActionRegisterer _registerer;
    };

    FCVStepRegistry() = default;
    virtual ~FCVStepRegistry();

    static FCVStepRegistry& get(ServiceContext* serviceContext);

    void prepareToUpgradeActionsBeforeGlobalLock(OperationContext* opCtx,
                                                 FCV originalVersion,
                                                 FCV requestedVersion) final;

    void userCollectionsUassertsForUpgrade(OperationContext* opCtx,
                                           FCV originalVersion,
                                           FCV requestedVersion) final;

    void userCollectionsWorkForUpgrade(OperationContext* opCtx,
                                       FCV originalVersion,
                                       FCV requestedVersion) final;

    void upgradeServerMetadata(OperationContext* opCtx,
                               FCV originalVersion,
                               FCV requestedVersion) final;

    void finalizeUpgrade(OperationContext* opCtx, FCV requestedVersion) final;

    void prepareToDowngradeActions(OperationContext* opCtx,
                                   FCV originalVersion,
                                   FCV requestedVersion) final;

    void userCollectionsUassertsForDowngrade(OperationContext* opCtx,
                                             FCV originalVersion,
                                             FCV requestedVersion) final;

    void internalServerCleanupForDowngrade(OperationContext* opCtx,
                                           FCV originalVersion,
                                           FCV requestedVersion) final;

    void finalizeDowngrade(OperationContext* opCtx, FCV requestedVersion) final;


    inline std::string getStepName() const final {
        return "FCVStepRegistry";
    }

private:
    void _registerFeature(FCVStep* step);
    void _unregisterFeature(FCVStep* step);

    std::vector<FCVStep*> _steps;
};

}  // namespace mongo
