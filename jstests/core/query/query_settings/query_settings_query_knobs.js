/**
 * Tests that the queryKnobs field in QuerySettings can be set and read back via
 * setQuerySettings/getQuerySettings.
 * @tags: [
 *   # TODO SERVER-98659 Investigate why this test is failing on
 *   # 'sharding_kill_stepdown_terminate_jscore_passthrough'.
 *   does_not_support_stepdowns,
 *   # Query settings commands can not be run on the shards directly.
 *   directly_against_shardsvrs_incompatible,
 *   # TODO(SERVER-113800): Enable setClusterParameters with replicaset started with --shardsvr
 *   transitioning_replicaset_incompatible,
 *   featureFlagPqsQueryKnobs,
 *   requires_fcv_90,
 * ]
 */

import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {after, describe, it} from "jstests/libs/mochalite.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());
const representativeQuery = qsutils.makeFindQueryInstance({filter: {x: 1}});

function assertQueryKnobsNotSettable(queryKnobs, errorCode) {
    const cmd = qsutils.makeSetQuerySettingsCommand({representativeQuery, settings: {queryKnobs}});
    assert.commandFailedWithCode(db.adminCommand(cmd), errorCode);
}

describe("setQuerySettings with queryKnobs", function () {
    after(function () {
        assertDropCollection(db, coll.getName());
        qsutils.removeAllQuerySettings();
    });

    it("Should reject unknown knob names", function () {
        assertQueryKnobsNotSettable({unknownKnob: 1}, 12194500);
    });

    it("Should reject knob values of the wrong type", function () {
        assertQueryKnobsNotSettable({samplingMarginOfError: "not a double"}, 12194501);
    });

    it("Should reject unknown enum values", function () {
        assertQueryKnobsNotSettable({planRankerMode: "notAValidPlanRankerMode"}, 12194501);
    });

    it("Should reject NaN knob values", function () {
        assertQueryKnobsNotSettable({samplingMarginOfError: NaN}, 12194501);
    });

    it("Should reject knobs not marked with pqs_settable: true", function () {
        assertQueryKnobsNotSettable({queryFrameworkControl: "trySbeEngine"}, 12194500);
    });

    it("Should round trip correctly", function () {
        const queryKnobs = {
            samplingMarginOfError: 3.0,
            planRankerMode: "histogramCE",
            automaticCEPlanRankingStrategy: "CBRCostBasedRankerChoice",
            samplingConfidenceInterval: "99",
            samplingCEMethod: "random",
        };
        const roundTrippedSettings = qsutils
            .withQuerySettings(representativeQuery, {queryKnobs}, () => {
                return qsutils.getQuerySettings({filter: {representativeQuery}});
            })
            .at(0)?.settings?.queryKnobs;
        assert(bsonWoCompare(roundTrippedSettings, queryKnobs) === 0, {
            roundTrippedSettings,
            queryKnobs,
        });
    });

    it("Should coexist with other settings", function () {
        const settings = {
            queryFramework: "classic",
            queryKnobs: {samplingMarginOfError: 5.0, planRankerMode: "samplingCE"},
        };
        const roundTrippedSettings = qsutils
            .withQuerySettings(representativeQuery, settings, () => {
                return qsutils.getQuerySettings({filter: {representativeQuery}});
            })
            .at(0)?.settings;
        assert(bsonWoCompare(roundTrippedSettings, settings) === 0, {
            roundTrippedSettings,
            settings,
        });
    });
});
