/**
 * Load this file when starting a mongo shell program in order to provide a callback to validate
 * collections and indexes before shutting down a mongod while running JS tests.
 */
import {validateCollections} from "jstests/hooks/validate_collections.js";
import {assertCatalogListOperationsConsistencyForDb} from "jstests/libs/catalog_list_operations_consistency_validator.js";
import {CommandSequenceWithRetries} from "jstests/libs/command_sequence_with_retries.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

MongoRunner.validateCollectionsCallback = function (port, options) {
    options = options || {};
    const CommandSequenceWithRetriesImpl = options.CommandSequenceWithRetries || CommandSequenceWithRetries;
    const validateCollectionsImpl = options.validateCollections || validateCollections;

    if (jsTest.options().skipCollectionAndIndexValidation) {
        jsTest.log.info("Skipping collection validation during mongod shutdown");
        return;
    }

    let conn;
    try {
        conn = new Mongo("localhost:" + port, undefined, {gRPC: false});
    } catch (e) {
        jsTest.log.info(
            "Skipping collection validation because we couldn't establish a connection to the" +
                " server on port " +
                port,
        );
        return;
    }

    // Set secondaryOk=true so that we can run commands against any secondaries.
    conn.setSecondaryOk();

    let dbs;
    let result = new CommandSequenceWithRetriesImpl(conn)
        .then("running the isMaster command", function (conn) {
            const res = assert.commandWorked(conn.adminCommand({isMaster: 1}));
            if (res.msg === "isdbgrid") {
                return {shouldStop: true, reason: "not running validate against mongos"};
            } else if (!res.ismaster && !res.secondary) {
                return {
                    shouldStop: true,
                    reason: "not running validate since mongod isn't in the PRIMARY" + " or SECONDARY states",
                };
            }
        })
        .then("authenticating", function (conn) {
            if (jsTest.options().keyFile) {
                jsTest.authenticate(conn);
            }
        })
        .then("best effort to step down node forever", function (conn) {
            if (conn.isReplicaSetMember()) {
                // This node should never run for election again. If the node has not
                // been initialized yet, then it cannot get elected.
                const kFreezeTimeSecs = 24 * 60 * 60; // 24 hours.

                assert.soon(
                    () => {
                        assert.commandWorkedOrFailedWithCode(
                            conn.adminCommand({replSetStepDown: kFreezeTimeSecs, force: true}),
                            [
                                ErrorCodes.NotWritablePrimary,
                                ErrorCodes.NotYetInitialized,
                                ErrorCodes.Unauthorized,
                                ErrorCodes.ConflictingOperationInProgress,
                                ErrorCodes.InterruptedDueToReplStateChange,
                                ErrorCodes.PrimarySteppedDown,
                            ],
                        );
                        const res = conn.adminCommand({replSetFreeze: kFreezeTimeSecs});
                        assert.commandWorkedOrFailedWithCode(res, [
                            ErrorCodes.NotYetInitialized,
                            ErrorCodes.Unauthorized,
                            ErrorCodes.NotSecondary,
                            ErrorCodes.InterruptedDueToReplStateChange,
                        ]);

                        // If 'replSetFreeze' succeeds or fails with NotYetInitialized or
                        // Unauthorized, we do not need to retry the command because
                        // retrying will not work if the replica set is not yet
                        // initialized or if we are not authorized to run the command.
                        // This is why this is a "best-effort".
                        if (res.ok === 1 || res.code !== ErrorCodes.NotSecondary) {
                            return true;
                        }

                        // We only retry on NotSecondary or
                        // InterruptedDueToReplStateChange error because 'replSetFreeze'
                        // could fail with NotSecondary or InterruptedDueToReplStateChange
                        // if there is a concurrent election running in parallel with the
                        // 'replSetStepDown' sent above.
                        jsTestLog(
                            "Retrying 'replSetStepDown' and 'replSetFreeze' in port " +
                                conn.port +
                                " res: " +
                                tojson(res),
                        );
                        return false;
                    },
                    "Timed out running 'replSetStepDown' and 'replSetFreeze' node in " + "port " + conn.port,
                );
            }
        })
        .then("getting the list of databases", function (conn) {
            const multitenancyRes = conn.adminCommand({getParameter: 1, multitenancySupport: 1});
            const multitenancy = multitenancyRes.ok && multitenancyRes["multitenancySupport"];

            const cmdObj = multitenancy ? {listDatabasesForAllTenants: 1} : {listDatabases: 1};
            const res = conn.adminCommand(cmdObj);
            if (!res.ok) {
                assert.commandFailedWithCode(res, ErrorCodes.Unauthorized);
                return {shouldStop: true, reason: "cannot run listDatabases"};
            }
            assert.commandWorked(res);
            dbs = res.databases.map((dbInfo) => {
                return {name: dbInfo.name, tenant: dbInfo.tenantId};
            });
        })
        .execute();

    if (!result.ok) {
        jsTest.log.info("Skipping collection validation: " + result.msg);
        return;
    }

    const cmds = new CommandSequenceWithRetriesImpl(conn);
    for (let i = 0; i < dbs.length; ++i) {
        const dbName = dbs[i].name;
        const tenant = dbs[i].tenant;
        cmds.then("validating " + dbName, function (conn) {
            const validateOptions = {
                full: true,
                enforceFastCount: true,
                checkBSONConformance: true,
            };
            // TODO (SERVER-24266): Once fast counts are tolerant to unclean shutdowns, remove the
            // check for TestData.allowUncleanShutdowns.
            if (TestData.skipEnforceFastCountOnValidate || TestData.allowUncleanShutdowns) {
                validateOptions.enforceFastCount = false;
            }

            try {
                const token = tenant ? _createTenantToken({tenant}) : undefined;
                conn._setSecurityToken(token);
                const validate_res = validateCollectionsImpl(conn.getDB(dbName), validateOptions);
                if (!validate_res.ok) {
                    return {
                        shouldStop: true,
                        reason: "collection validation failed " + tojson(validate_res),
                    };
                }

                try {
                    // The replica set endpoint of a single-shard cluster with config shard
                    // can currently become unavailable if a majority of nodes steps down.
                    // Skip the catalog consistency check it may not be able to read the catalog.
                    // TODO(SERVER-98707): Don't skip the catalog consistency check
                    const skipCatalogConsistencyChecker =
                        TestData.configShard && FeatureFlagUtil.isEnabled(conn, "ReplicaSetEndpoint");
                    if (!skipCatalogConsistencyChecker) {
                        assertCatalogListOperationsConsistencyForDb(conn.getDB(dbName), tenant);
                    }
                } catch (e) {
                    return {
                        shouldStop: true,
                        reason: "catalog list operations consistency check failed " + tojson(e),
                    };
                }
            } finally {
                conn._setSecurityToken(undefined);
            }
        });
    }

    assert.commandWorked(cmds.execute());
};
