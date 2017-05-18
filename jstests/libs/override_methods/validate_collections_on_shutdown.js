/**
 * Load this file when starting a mongo shell program in order to provide a callback to validate
 * collections and indexes before shutting down a mongod while running JS tests.
 */

(function() {
    "use strict";

    load("jstests/libs/command_sequence_with_retries.js");  // for CommandSequenceWithRetries

    MongoRunner.validateCollectionsCallback = function(port) {
        if (jsTest.options().skipCollectionAndIndexValidation) {
            print("Skipping collection validation during mongod shutdown");
            return;
        }

        let conn;
        try {
            conn = new Mongo("localhost:" + port);
        } catch (e) {
            print(
                "Skipping collection validation because we couldn't establish a connection to the" +
                " server on port " + port);
            return;
        }

        // Set slaveOk=true so that we can run commands against any secondaries.
        conn.setSlaveOk();

        let dbNames;
        let result =
            new CommandSequenceWithRetries(conn)
                .then("running the isMaster command",
                      function(conn) {
                          const res = assert.commandWorked(conn.adminCommand({isMaster: 1}));
                          if (res.msg === "isdbgrid") {
                              return {
                                  shouldStop: true,
                                  reason: "not running validate against mongos"
                              };
                          } else if (!res.ismaster && !res.secondary) {
                              return {
                                  shouldStop: true,
                                  reason: "not running validate since mongod isn't in the PRIMARY" +
                                      " or SECONDARY states"
                              };
                          }
                      })
                .then("authenticating",
                      function(conn) {
                          if (jsTest.options().keyFile) {
                              jsTest.authenticate(conn);
                          }
                      })
                .then("getting the list of databases",
                      function(conn) {
                          const res = conn.adminCommand({listDatabases: 1});
                          if (!res.ok) {
                              assert.commandFailedWithCode(res, ErrorCodes.Unauthorized);
                              return {
                                  shouldStop: true,
                                  reason: "not authorized to run listDatabases"
                              };
                          }
                          assert.commandWorked(res);
                          dbNames = res.databases.map(dbInfo => dbInfo.name);
                      })
                .execute();

        if (!result.ok) {
            print("Skipping collection validation: " + result.msg);
            return;
        }

        load('jstests/hooks/validate_collections.js');  // for validateCollections

        const cmds = new CommandSequenceWithRetries(conn);
        for (let dbName of dbNames) {
            cmds.then("DEBUG: validating " + dbName, function(conn) {
                if (!validateCollections(conn.getDB(dbName), {full: true})) {
                    return {shouldStop: true, reason: "collection validation failed"};
                }
            });
        }

        result = cmds.execute();
        if (!result.ok) {
            throw new Error(result.reason);
        }
    };
})();
