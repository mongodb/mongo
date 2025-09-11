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
 *      # Query settings commands can not be handled by atlas proxy.
 *      simulate_atlas_proxy_incompatible,
 * ]
 */

import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {kRawOperationFieldName} from "jstests/libs/raw_operation_utils.js";

const collName = jsTestName();
const qsutils = new QuerySettingsUtils(db, collName);
qsutils.removeAllQuerySettings();

const settings = {
    queryFramework: "classic",
};

function runTest(queryInstance) {
    // Run each command 3 times: Once with no rawData parameter, once with rawData: true, and once with rawData: false.
    const commandVariations = [
        buildQuerySettingsCommand(queryInstance, null),
        buildQuerySettingsCommand(queryInstance, true),
        buildQuerySettingsCommand(queryInstance, false),
    ];

    print("output commands:");
    printjson(commandVariations);

    commandVariations.forEach((command) => {
        printjson(command);
        // Run setQuerySettings command.
        // If the rawData param exists and is true, we expect to fail. Otherwise, we expect the command to succeed.
        if (command["setQuerySettings"][kRawOperationFieldName]) {
            assert.commandFailedWithCode(
                db.adminCommand(command),
                1064380,
                "setQuerySetting command cannot be used with rawData enabled",
            );
        } else {
            assert.commandWorked(db.adminCommand(command));
        }
    });
}

function buildQuerySettingsCommand(queryInstance, rawDataValue) {
    // Make a copy of the command so we don't modify the original.
    // Otherwise, subsequent calls to this function will alter the query settings objects produced by previous calls.
    let updatedQuery = {...queryInstance};

    if (rawDataValue == null) {
        return {setQuerySettings: updatedQuery, settings: settings};
    } else {
        updatedQuery[kRawOperationFieldName] = rawDataValue;
        return {setQuerySettings: updatedQuery, settings: settings};
    }
}

// Test find command with no rawData parameter, with rawData: true, and with rawData: false.
runTest(qsutils.makeFindQueryInstance({filter: {evil: true}}));

// Test aggregation command with no rawData parameter, with rawData: true, and with rawData: false.
runTest(
    qsutils.makeAggregateQueryInstance(
        {
            pipeline: [{$querySettings: {showDebugQueryShape: true}}],
        },
        /* collectionless */ true,
    ),
);

// Test distinct command with no rawData parameter, with rawData: true, and with rawData: false.
runTest(qsutils.makeDistinctQueryInstance({key: "a"}));
