/**
 * Verifies that setQuerySettings fails when the command has the rawData parameter set to true
 *
 *
 * @tags: [
 *      does_not_support_stepdowns,
 *      # Can't run multiversion tests because the rawData parameter isn't supported
 *      requires_fcv_83,
 *      # Query settings commands can not be run on the shards directly.
 *      directly_against_shardsvrs_incompatible,
 *      # TODO(SERVER-113800): Enable setClusterParameters with replicaset started with --shardsvr
 *      transitioning_replicaset_incompatible,
 *      incompatible_with_views,
 * ]
 */

import {describe, it, before, after} from "jstests/libs/mochalite.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {kRawOperationFieldName} from "jstests/libs/raw_operation_utils.js";

function assertQuerySettingsRejectFlagIsSuccesfullyApplied(qsutils, queryInstance) {
    const db = qsutils._db;
    qsutils.withQuerySettings(queryInstance, {reject: true}, () => {
        // Ensure that the query is rejected due to the query settings, which confirms that the settings were applied successfully.
        const cmdObj = qsutils.withoutDollarDB(queryInstance);
        assert.commandFailedWithCode(db.runCommand(cmdObj), [ErrorCodes.QueryRejectedBySettings]);
    });
}

function assertSetQuerySettingsByQueryValidationFailure(qsutils, queryInstance) {
    // Try to set query settings and expect the setQuerySettings command to fail the validation.
    const db = qsutils._db;
    const res = db.adminCommand({setQuerySettings: queryInstance, settings: {reject: true}});
    assert.commandFailedWithCode(res, [1064380]);
}

function assertQuerySettingsRejectFlagIsNotApplied(qsutils, queryInstance) {
    const db = qsutils._db;
    const queryShapeHash = qsutils.getQueryShapeHashFromExplain(queryInstance);
    qsutils.withQuerySettings(queryShapeHash, {reject: true}, () => {
        const cmdObj = qsutils.withoutDollarDB(queryInstance);
        assert.commandWorked(db.runCommand(cmdObj));
    });
}

function assertQueriesHaveDifferetQueryShapeHashes(qsutils, queryInstance0, queryInstance1) {
    const hash0 = qsutils.getQueryShapeHashFromExplain(queryInstance0);
    const hash1 = qsutils.getQueryShapeHashFromExplain(queryInstance1);
    assert.neq(
        hash0,
        hash1,
        `Expected different query shape hashes for ${tojson(queryInstance0)} and ${tojson(queryInstance1)}`,
    );
}

describe("setQuerySettings commands against", function () {
    const rawDataOpt = (value) => {
        return {[kRawOperationFieldName]: value};
    };

    before(function () {
        this.collection = assertDropAndRecreateCollection(db, jsTestName());
        this.qsutils = new QuerySettingsUtils(db, this.collection.getName());
        this.findQueryInstance = this.qsutils.makeFindQueryInstance({filter: {}});
        this.aggQueryInstance = this.qsutils.makeAggregateQueryInstance({pipeline: []});
        this.distinctQueryInstance = this.qsutils.makeDistinctQueryInstance({key: "a"});
    });

    after(function () {
        assert(this.collection.drop());
    });

    it("find commands with rawData=false should apply query settings", function () {
        assertQuerySettingsRejectFlagIsSuccesfullyApplied(this.qsutils, {
            ...this.findQueryInstance,
            ...rawDataOpt(false),
        });
    });

    it("find commands with rawData=true should not apply query settings", function () {
        assertSetQuerySettingsByQueryValidationFailure(this.qsutils, {...this.findQueryInstance, ...rawDataOpt(true)});
        // TODO SERVER-112940: Enable this test after the find command is updated to include the rawData parameter in its query shape hash calculation.
        // assertQuerySettingsRejectFlagIsNotApplied(this.qsutils, {...this.findQueryInstance, ...rawDataOpt(true)});
    });

    // TODO SERVER-112940: Enable this test after the find command is updated to include the rawData parameter in the query shape hash calculation.
    // it("find commands with different rawData values should have different query shape hashes", function () {
    //     assertQueriesHaveDifferetQueryShapeHashes(
    //         this.qsutils,
    //         {...this.findQueryInstance, ...rawDataOpt(false)},
    //         {...this.findQueryInstance, ...rawDataOpt(true)},
    //     );
    // });

    it("aggregate commands with rawData=false should apply query settings", function () {
        assertQuerySettingsRejectFlagIsSuccesfullyApplied(this.qsutils, {
            ...this.aggQueryInstance,
            ...rawDataOpt(false),
        });
    });

    it("aggregate commands with rawData=true should not apply query settings", function () {
        assertSetQuerySettingsByQueryValidationFailure(this.qsutils, {...this.aggQueryInstance, ...rawDataOpt(true)});
        // TODO SERVER-112940: Enable this test after the aggregate command is updated to include the rawData parameter in its query shape hash calculation.
        // assertQuerySettingsRejectFlagIsNotApplied(this.qsutils, {...this.aggQueryInstance, ...rawDataOpt(true)});
    });

    // TODO SERVER-112940: Enable this test after the aggregate command is updated to include the rawData parameter in the query shape hash computation.
    // it("aggregate commands with different rawData values should have different query shape hashes", function () {
    //     assertQueriesHaveDifferetQueryShapeHashes(
    //         this.qsutils,
    //         {...this.aggQueryInstance, ...rawDataOpt(false)},
    //         {...this.aggQueryInstance, ...rawDataOpt(true)},
    //     );
    // });

    it("distinct commands with rawData=false should apply query settings", function () {
        assertQuerySettingsRejectFlagIsSuccesfullyApplied(this.qsutils, {
            ...this.distinctQueryInstance,
            ...rawDataOpt(false),
        });
    });

    it("distinct commands with rawData=true should not apply query settings", function () {
        assertSetQuerySettingsByQueryValidationFailure(this.qsutils, {
            ...this.distinctQueryInstance,
            ...rawDataOpt(true),
        });
        // TODO SERVER-112940: Enable this test after the distinct command is updated to include the rawData parameter in its query shape hash calculation.
        // assertQuerySettingsRejectFlagIsNotApplied(this.qsutils, {...this.distinctQueryInstance, ...rawDataOpt(true)});
    });

    // TODO SERVER-112940: Enable this test after the distinct command is updated to include the rawData parameter in its query shape hash calculation.
    // it("distinct commands with different rawData values should have different query shape hashes", function () {
    //     assertQueriesHaveDifferetQueryShapeHashes(
    //         this.qsutils,
    //         {...this.distinctQueryInstance, ...rawDataOpt(false)},
    //         {...this.distinctQueryInstance, ...rawDataOpt(true)},
    //     );
    // });
});
