// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/feature_compatibility_version_document_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/version/releases.h"

namespace mongo {
namespace {

using FCV = multiversion::FeatureCompatibilityVersion;
using GenericFCV = multiversion::GenericFCV;

// Scenarios are expressed as FCV documents (the shape the server persists) and applied via
// setVersionFromFCVDocument(fcvDoc), matching how the in-memory FCV is actually populated in
// production.
static FeatureCompatibilityVersionDocument makeFCVDoc(FCV version,
                                                      boost::optional<FCV> targetVersion,
                                                      boost::optional<FCV> previousVersion,
                                                      boost::optional<SetFCVPhaseEnum> phase) {
    FeatureCompatibilityVersionDocument doc;
    doc.setVersion(version);
    doc.setTargetVersion(targetVersion);
    doc.setPreviousVersion(previousVersion);
    doc.setPhase(phase);
    return doc;
}

struct MetricsScenario {
    std::string name;
    FeatureCompatibilityVersionDocument doc;
    int expectedTransitioning;
};

// Drives the real "featureCompatibilityVersion" serverStatus section (looked up from the registry)
// rather than any extracted helper, so the test exercises the field exactly as it is emitted.
class FCVMetricsTest : public ServiceContextTest,
                       public testing::WithParamInterface<MetricsScenario> {
public:
    // The registered FCV serverStatus section, or nullptr if it is not registered.
    static ServerStatusSection* findFCVSection() {
        auto* registry = ServerStatusSectionRegistry::instance();
        for (auto it = registry->begin(); it != registry->end(); ++it) {
            if (it->second->getSectionName() == "featureCompatibilityVersion") {
                return it->second.get();
            }
        }
        return nullptr;
    }

    // Runs the section against the current in-memory FCV and returns its "transitioning" field.
    int runTransitioning() {
        auto* section = findFCVSection();
        ASSERT(section);
        auto opCtx = makeOperationContext();
        const auto obj = section->generateSection(opCtx.get(), BSONElement{});
        return obj["transitioning"].numberInt();
    }
};

TEST_P(FCVMetricsTest, TransitioningField) {
    const auto& s = GetParam();

    serverGlobalParams.mutableFCV.setVersionFromFCVDocument(s.doc);

    ASSERT_EQ(runTransitioning(), s.expectedTransitioning);
}

INSTANTIATE_TEST_SUITE_P(
    TransitioningField,
    FCVMetricsTest,
    testing::ValuesIn(std::vector<MetricsScenario>{
        {"SteadyState", makeFCVDoc(GenericFCV::kLatest, boost::none, boost::none, boost::none), 0},
        {"EarlyUpgrade",
         makeFCVDoc(GenericFCV::kLastLTS, GenericFCV::kLatest, boost::none, boost::none),
         1},
        {"LateUpgrade",
         makeFCVDoc(GenericFCV::kLatest,
                    GenericFCV::kLatest,
                    GenericFCV::kLastLTS,
                    SetFCVPhaseEnum::kEnableTargetFeatures),
         1},
        {"Downgrade",
         makeFCVDoc(GenericFCV::kLastLTS, GenericFCV::kLastLTS, GenericFCV::kLatest, boost::none),
         -1},
    }),
    [](const testing::TestParamInfo<MetricsScenario>& info) { return info.param.name; });

}  // namespace
}  // namespace mongo
