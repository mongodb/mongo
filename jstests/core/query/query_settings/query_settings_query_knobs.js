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

function mergeQuerySettings(initialSettings, newSettings) {
    return qsutils.withQuerySettings(representativeQuery, initialSettings, () =>
        qsutils.withQuerySettings(representativeQuery, newSettings, () => {
            const configuration = qsutils.getQuerySettings({filter: {representativeQuery}});
            return configuration.at(0)?.settings;
        }),
    );
}

function assertEqWo(actual, expected, opts) {
    return assert(bsonWoCompare(actual, expected) === 0, opts);
}

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
        assertQueryKnobsNotSettable({unknownKnob: 1}, 12194501);
    });

    it("Should reject knob values of the wrong type", function () {
        assertQueryKnobsNotSettable({samplingMarginOfError: "not a double"}, 12194501);
    });

    it("Should reject unknown enum values", function () {
        assertQueryKnobsNotSettable({planRanker: "notAValidPlanRankerMode"}, 12194501);
    });

    it("Should reject NaN knob values", function () {
        assertQueryKnobsNotSettable({samplingMarginOfError: NaN}, 12194501);
    });

    it("Should reject samplingMarginOfError below minimum (< 1.0)", function () {
        assertQueryKnobsNotSettable({samplingMarginOfError: 0.5}, 12194501);
    });

    it("Should reject samplingMarginOfError above maximum (> 10.0)", function () {
        assertQueryKnobsNotSettable({samplingMarginOfError: 10.1}, 12194501);
    });

    it("Should reject knobs not marked with pqs_settable: true", function () {
        assertQueryKnobsNotSettable({queryFrameworkControl: "trySbeEngine"}, 12194501);
    });

    it("Should accept bool values for noTableScan", function () {
        for (const value of [true, false]) {
            const stored = qsutils
                .withQuerySettings(representativeQuery, {queryKnobs: {noTableScan: value}}, () => {
                    return qsutils.getQuerySettings({filter: {representativeQuery}});
                })
                .at(0)?.settings?.queryKnobs?.noTableScan;
            assert.eq(stored, value, {value, stored});
        }
    });

    it("Should round trip correctly", function () {
        const queryKnobs = {
            samplingMarginOfError: 3.0,
            planRanker: "mixed",
            cbrCEMode: "histogramCE",
            automaticCEPlanRankingStrategy: "CBRCostBasedRankerChoice",
            samplingConfidenceInterval: "99",
            samplingCEMethod: "random",
            noTableScan: false,
        };
        const roundTrippedSettings = qsutils
            .withQuerySettings(representativeQuery, {queryKnobs}, () => {
                return qsutils.getQuerySettings({filter: {representativeQuery}});
            })
            .at(0)?.settings?.queryKnobs;
        assertEqWo(roundTrippedSettings, queryKnobs, {
            roundTrippedSettings,
            queryKnobs,
        });
    });

    it("Should coexist with other settings", function () {
        const settings = {
            queryFramework: "classic",
            queryKnobs: {
                samplingMarginOfError: 5.0,
                planRanker: "costBased",
                cbrCEMode: "samplingCE",
            },
        };
        const roundTrippedSettings = qsutils
            .withQuerySettings(representativeQuery, settings, () => {
                return qsutils.getQuerySettings({filter: {representativeQuery}});
            })
            .at(0)?.settings;
        assertEqWo(roundTrippedSettings, settings, {
            roundTrippedSettings,
            settings,
        });
    });

    it("Should update an existing knob value without disturbing others", function () {
        const initialSettings = {
            queryKnobs: {
                samplingMarginOfError: 3.0,
                planRanker: "costBased",
                cbrCEMode: "histogramCE",
            },
        };
        const updateSettings = {queryKnobs: {samplingMarginOfError: 5.0}};
        const expectedKnobs = {
            samplingMarginOfError: 5.0,
            planRanker: "costBased",
            cbrCEMode: "histogramCE",
        };
        const actualKnobs = mergeQuerySettings(initialSettings, updateSettings)?.queryKnobs;
        assertEqWo(actualKnobs, expectedKnobs, {actualKnobs, expectedKnobs});
    });

    it("Should remove a single knob by setting it to null, preserving the rest", function () {
        const initialSettings = {
            queryKnobs: {samplingMarginOfError: 3.0, samplingConfidenceInterval: "99"},
        };
        const removalSettings = {queryKnobs: {samplingMarginOfError: null}};
        const expectedKnobs = {samplingConfidenceInterval: "99"};
        const actualKnobs = mergeQuerySettings(initialSettings, removalSettings)?.queryKnobs;
        assertEqWo(actualKnobs, expectedKnobs, {actualKnobs, expectedKnobs});
    });

    it("Should remove one knob while setting and updating others in the same command", function () {
        const initialSettings = {
            queryKnobs: {samplingMarginOfError: 3.0, samplingConfidenceInterval: "99"},
        };
        // Remove samplingMarginOfError, update samplingConfidenceInterval, add planRanker.
        const updateSettings = {
            queryKnobs: {
                samplingMarginOfError: null,
                samplingConfidenceInterval: "95",
                planRanker: "costBased",
            },
        };
        const expectedKnobs = {planRanker: "costBased", samplingConfidenceInterval: "95"};
        const actualKnobs = mergeQuerySettings(initialSettings, updateSettings)?.queryKnobs;
        assertEqWo(actualKnobs, expectedKnobs, {actualKnobs, expectedKnobs});
    });

    it("Should drop the queryKnobs field when all knobs are removed via null", function () {
        const initialSettings = {
            queryFramework: "classic",
            queryKnobs: {samplingMarginOfError: 3.0, samplingConfidenceInterval: "99"},
        };
        // Null out every currently-set knob in a single command.
        const removalSettings = {
            queryKnobs: {samplingMarginOfError: null, samplingConfidenceInterval: null},
        };
        const actualSettings = mergeQuerySettings(initialSettings, removalSettings);
        assertEqWo(actualSettings, {queryFramework: "classic"}, {actualSettings});
    });

    it("Should reject INSERT or UPDATE that would leave settings empty", function () {
        // INSERT: null-only queryKnobs as the only setting fails.
        assert.commandFailedWithCode(
            db.adminCommand({
                setQuerySettings: representativeQuery,
                settings: {queryKnobs: {samplingMarginOfError: null}},
            }),
            7746604,
        );

        // Non-null INSERT works; UPDATE that nulls the only knob also fails.
        qsutils.withQuerySettings(
            representativeQuery,
            {queryKnobs: {samplingMarginOfError: 3.0}},
            () => {
                assert.commandFailedWithCode(
                    db.adminCommand({
                        setQuerySettings: representativeQuery,
                        settings: {queryKnobs: {samplingMarginOfError: null}},
                    }),
                    7746604,
                );
            },
        );
    });
});
