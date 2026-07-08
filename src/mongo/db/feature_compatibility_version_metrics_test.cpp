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
