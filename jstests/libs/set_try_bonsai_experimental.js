/**
 * Set internalQueryFrameworkControl to tryBonsaiExperimental and
 * internalQueryCardinalityEstimatorMode to sampling. This is intended to be used by tasks which
 * should use experimental bonsai behavior, currently defined by both the control knob and the CE
 * mode, regardless of the configuration of the variant running the task. This is needed because the
 * suite definition cannot override a knob which is also defined by the variant.
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

if (typeof db !== "undefined") {
    FixtureHelpers.mapOnEachShardNode({
        db: db,
        func: (db) => {
            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                internalQueryFrameworkControl: "tryBonsaiExperimental",
                internalQueryCardinalityEstimatorMode: "sampling"
            }));
        },
        primaryNodeOnly: false,
    });
}

if (typeof TestData !== "undefined") {
    if (!TestData.hasOwnProperty("setParameters")) {
        TestData["setParameters"] = {};
    }
    if (!TestData.hasOwnProperty("setParametersMongos")) {
        TestData["setParametersMongos"] = {};
    }

    TestData["setParameters"]["internalQueryFrameworkControl"] = "tryBonsaiExperimental";
    TestData["setParametersMongos"]["internalQueryFrameworkControl"] = "tryBonsaiExperimental";

    TestData["setParameters"]["internalQueryCardinalityEstimatorMode"] = "sampling";
    TestData["setParametersMongos"]["internalQueryCardinalityEstimatorMode"] = "sampling";
}
