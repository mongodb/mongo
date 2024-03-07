/**
 * Tests that modifying query settings is not allowed on standalone (because of the missing
 * 'VectorClock').
 * @tags: [
 *   featureFlagQuerySettings,
 * ]
 */

import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

const qsutils = new QuerySettingsUtils(db, jsTestName());
const querySettingsQuery = qsutils.makeFindQueryInstance({filter: {a: 1, b: 1}, skip: 3});
const querySettingsSettings = {
    indexHints: {allowedIndexes: ["a_1"]}
};

(function setQuerySettingsFailsOnStandalone() {
    assert.commandFailedWithCode(
        db.adminCommand({setQuerySettings: querySettingsQuery, settings: querySettingsSettings}),
        ErrorCodes.IllegalOperation);
})();

(function removeQuerySettingsFailsOnStandalone() {
    assert.commandFailedWithCode(db.adminCommand({removeQuerySettings: querySettingsQuery}),
                                 ErrorCodes.IllegalOperation);
})();
