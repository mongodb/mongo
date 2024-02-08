/**
 * Class containing common test functions used in query_settings_index_application_* tests.
 */

export class QuerySettingsIndexHintsTests {
    /**
     * Create a query settings utility class.
     */
    constructor(qsutils) {
        this.qsutils = qsutils;
        this.indexA = {a: 1};
        this.indexB = {b: 1};
        this.indexAB = {a: 1, b: 1};
    }

    // Ensure query settings are applied as expected in a straightforward scenario.
    testQuerySettingsIndexApplication(querySettingsQuery) {
        const query = this.qsutils.withoutDollarDB(querySettingsQuery);
        for (const index of [this.indexA, this.indexB, this.indexAB]) {
            const settings = {indexHints: {allowedIndexes: [index]}};
            this.qsutils.withQuerySettings(querySettingsQuery, settings, () => {
                this.qsutils.assertIndexScanStage(query, index);
                this.qsutils.assertQuerySettingsInCacheForCommand(query, settings);
            });
        }
    }

    // Ensure query settings '$natural' hints are applied as expected in a straightforward scenario.
    // This test case covers the following scenarios:
    //     * Only forward scans allowed.
    //     * Only backward scans allowed.
    //     * Both forward and backward scans allowed.
    testQuerySettingsNaturalApplication(querySettingsQuery) {
        const query = this.qsutils.withoutDollarDB(querySettingsQuery);
        const naturalForwardScan = {$natural: 1};
        const naturalForwardSettings = {indexHints: {allowedIndexes: [naturalForwardScan]}};
        this.qsutils.withQuerySettings(querySettingsQuery, naturalForwardSettings, () => {
            this.qsutils.assertCollScanStage(query, ["forward"]);
            this.qsutils.assertQuerySettingsInCacheForCommand(query, naturalForwardSettings);
        });

        const naturalBackwardScan = {$natural: -1};
        const naturalBackwardSettings = {indexHints: {allowedIndexes: [naturalBackwardScan]}};
        this.qsutils.withQuerySettings(querySettingsQuery, naturalBackwardSettings, () => {
            this.qsutils.assertCollScanStage(query, ["backward"]);
            this.qsutils.assertQuerySettingsInCacheForCommand(query, naturalBackwardSettings);
        });

        const naturalAnyDirectionSettings = {
            indexHints: {allowedIndexes: [naturalForwardScan, naturalBackwardScan]}
        };
        this.qsutils.withQuerySettings(querySettingsQuery, naturalAnyDirectionSettings, () => {
            this.qsutils.assertCollScanStage(query, ["forward", "backward"]);
            this.qsutils.assertQuerySettingsInCacheForCommand(query, naturalAnyDirectionSettings);
        });
    }

    // Ensure that the hint gets ignored when query settings for the particular query are set.
    testQuerySettingsIgnoreCursorHints(querySettingsQuery) {
        const query = this.qsutils.withoutDollarDB(querySettingsQuery);
        const settings = {indexHints: {allowedIndexes: [this.indexAB]}};
        const queryWithHint = {...query, hint: this.indexA};
        this.qsutils.withQuerySettings(querySettingsQuery, settings, () => {
            this.qsutils.assertIndexScanStage(queryWithHint, this.indexAB);
            this.qsutils.assertQuerySettingsInCacheForCommand(queryWithHint, settings);
        });
    }

    // Ensure that queries fall back to multiplanning when the provided settings don't generate any
    // viable plans. Limit the query to an index which does not exist and expect it to use any other
    // available index.
    testQuerySettingsFallback(querySettingsQuery) {
        const query = this.qsutils.withoutDollarDB(querySettingsQuery);
        const settings = {indexHints: {allowedIndexes: ["doesnotexist"]}};
        this.qsutils.withQuerySettings(querySettingsQuery, settings, () => {
            const expectedIndex = undefined;
            this.qsutils.assertIndexScanStage(query, expectedIndex);
            this.qsutils.assertQuerySettingsInCacheForCommand(query, settings);
        });
    }

    // Ensure that users can not pass query settings to the commands explicitly.
    testQuerySettingsCommandValidation(querySettingsQuery) {
        const query = this.qsutils.withoutDollarDB(querySettingsQuery);
        const settings = {indexHints: {allowedIndexes: [this.indexAB]}};
        const expectedErrorCodes = [7746900, 7746901, 7923000, 7923001, 7708000, 7708001];
        assert.commandFailedWithCode(db.runCommand({...query, querySettings: settings}),
                                     expectedErrorCodes);
    }
}
