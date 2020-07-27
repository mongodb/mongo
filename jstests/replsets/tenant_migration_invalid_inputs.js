/**
 * Tests that the donorStartMigration command does not allow users to provide a 'databasePrefix'
 * that is unsupported. The unsupported prefixes are: '', 'admin', 'local', 'config'.
 *
 * @tags: [requires_fcv_46]
 */

(function() {
"use strict";

const runDonorStartMigrationCommand =
    (primaryConnection, migrationId, recipientConnectionString, dbPrefix, readPreference) => {
        return primaryConnection.adminCommand({
            donorStartMigration: 1,
            migrationId,
            recipientConnectionString,
            databasePrefix: dbPrefix,
            readPreference
        });
    };
const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primaryConnection = rst.getPrimary();

const migrationId = new UUID();
const recipientConnectionString = 'placeholderURI';
const readPreference = {
    mode: "primary"
};

const unsupportedDBPrefixes = ['', 'admin', 'local', 'config'];

jsTest.log('Attempting donorStartMigration with unsupported databasePrefixes.');
unsupportedDBPrefixes.forEach((dbPrefix) => {
    assert.commandFailedWithCode(
        runDonorStartMigrationCommand(
            primaryConnection, migrationId, recipientConnectionString, dbPrefix, readPreference),
        ErrorCodes.BadValue);
});

rst.stopSet();
})();
