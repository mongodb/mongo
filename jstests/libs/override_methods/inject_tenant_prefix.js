/**
 * Overrides the database name of each accessed database ("config", "admin", "local" excluded) to
 * have the prefix TestData.tenantId so that the accessed data will be migrated by the background
 * tenant migrations run by the ContinuousTenantMigration hook.
 */
(function() {
'use strict';

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.
load("jstests/libs/transactions_util.js");

// Save references to the original methods in the IIFE's scope.
// This scoping allows the original methods to be called by the overrides below.
// Override this method to make the accessed database have the prefix TestData.tenantId.
let originalRunCommand = Mongo.prototype.runCommand;

const blacklistedDbNames = ["config", "admin", "local"];

function isBlacklistedDb(dbName) {
    return blacklistedDbNames.includes(dbName);
}

/**
 * If the database with the given name can be migrated, prepends TestData.tenantId to the name if
 * it does not already start with the prefix.
 */
function prependTenantIdToDbNameIfApplicable(dbName) {
    if (dbName.length === 0) {
        // There are input validation tests that use invalid database names, those should be
        // ignored.
        return dbName;
    }
    return isBlacklistedDb(dbName) ? dbName : TestData.tenantId + "_" + dbName;
}

/**
 * If the database for the given namespace can be migrated, prepends TestData.tenantId to the
 * namespace if it does not already start with the prefix.
 */
function prependTenantIdToNsIfApplicable(ns) {
    if (ns.length === 0 || !ns.includes(".")) {
        // There are input validation tests that use invalid namespaces, those should be ignored.
        return ns;
    }
    let splitNs = ns.split(".");
    splitNs[0] = prependTenantIdToDbNameIfApplicable(splitNs[0]);
    return splitNs.join(".");
}

/**
 * If the given database name starts TestData.tenantId, removes the prefix.
 */
function extractOriginalDbName(dbName) {
    return dbName.replace(TestData.tenantId + "_", "");
}

/**
 * If the database name for the given namespace starts TestData.tenantId, removes the prefix.
 */
function extractOriginalNs(ns) {
    let splitNs = ns.split(".");
    splitNs[0] = extractOriginalDbName(splitNs[0]);
    return splitNs.join(".");
}

/**
 * Removes all occurrences of TestDatabase.tenantId in the string.
 */
function removeTenantIdFromString(string) {
    return string.replace(new RegExp(TestData.tenantId + "_", "g"), "");
}

/**
 * Prepends TestDatabase.tenantId to all the database name and namespace fields inside the given
 * object.
 */
function prependTenantId(obj) {
    for (let k of Object.keys(obj)) {
        let v = obj[k];
        if (typeof v === "string") {
            if (k === "dbName" || k == "db") {
                obj[k] = prependTenantIdToDbNameIfApplicable(v);
            } else if (k === "namespace" || k === "ns") {
                obj[k] = prependTenantIdToNsIfApplicable(v);
            }
        } else if (Array.isArray(v)) {
            obj[k] = v.map((item) => {
                return (typeof item === "object" && item !== null) ? prependTenantId(item) : item;
            });
        } else if (typeof v === "object" && v !== null && Object.keys(v).length > 0) {
            obj[k] = prependTenantId(v);
        }
    }
    return obj;
}

/**
 * Removes TestDatabase.tenantId from all the database name and namespace fields inside the given
 * object.
 */
function removeTenantId(obj) {
    for (let k of Object.keys(obj)) {
        let v = obj[k];
        let originalK = removeTenantIdFromString(k);
        if (typeof v === "string") {
            if (k === "dbName" || k == "db" || k == "dropped") {
                obj[originalK] = extractOriginalDbName(v);
            } else if (k === "namespace" || k === "ns") {
                obj[originalK] = extractOriginalNs(v);
            } else if (k === "errmsg" || k == "name") {
                obj[originalK] = removeTenantIdFromString(v);
            }
        } else if (Array.isArray(v)) {
            obj[originalK] = v.map((item) => {
                return (typeof item === "object" && item !== null) ? removeTenantId(item) : item;
            });
        } else if (typeof v === "object" && v !== null && Object.keys(v).length > 0) {
            obj[originalK] = removeTenantId(v);
        }
    }
    return obj;
}

const kCmdsWithNsAsFirstField =
    new Set(["renameCollection", "checkShardingIndex", "dataSize", "datasize", "splitVector"]);

/**
 * Returns a new cmdObj with TestData.tenantId prepended to all database name and namespace fields.
 */
function createCmdObjWithTenantId(cmdObj) {
    const cmdName = Object.keys(cmdObj)[0];
    let cmdObjWithTenantId = TransactionsUtil.deepCopyObject({}, cmdObj);

    // Handle commands with special database and namespace field names.
    if (kCmdsWithNsAsFirstField.has(cmdName)) {
        cmdObjWithTenantId[cmdName] = prependTenantIdToNsIfApplicable(cmdObjWithTenantId[cmdName]);
    }

    switch (cmdName) {
        case "renameCollection":
            cmdObjWithTenantId.to = prependTenantIdToNsIfApplicable(cmdObjWithTenantId.to);
            break;
        case "internalRenameIfOptionsAndIndexesMatch":
            cmdObjWithTenantId.from = prependTenantIdToNsIfApplicable(cmdObjWithTenantId.from);
            cmdObjWithTenantId.to = prependTenantIdToNsIfApplicable(cmdObjWithTenantId.to);
            break;
        case "configureFailPoint":
            if (cmdObjWithTenantId.data) {
                if (cmdObjWithTenantId.data.namespace) {
                    cmdObjWithTenantId.data.namespace =
                        prependTenantIdToNsIfApplicable(cmdObjWithTenantId.data.namespace);
                } else if (cmdObjWithTenantId.data.ns) {
                    cmdObjWithTenantId.data.ns =
                        prependTenantIdToNsIfApplicable(cmdObjWithTenantId.data.ns);
                }
            }
            break;
        case "applyOps":
            for (let op of cmdObjWithTenantId.applyOps) {
                if (typeof op.ns === "string" && op.ns.endsWith("system.views") && op.o._id &&
                    typeof op.o._id === "string") {
                    // For views, op.ns and op.o._id must be equal.
                    op.o._id = prependTenantIdToNsIfApplicable(op.o._id);
                }
            }
            break;
        default:
            break;
    }

    // Recursively override the database name and namespace fields. Exclude 'configureFailPoint'
    // since data.errorExtraInfo.namespace or data.errorExtraInfo.ns can sometimes refer to
    // collection name instead of namespace.
    if (cmdName != "configureFailPoint") {
        prependTenantId(cmdObjWithTenantId);
    }

    return cmdObjWithTenantId;
}

Mongo.prototype.runCommand = function(dbName, cmdObj, options) {
    // Create another cmdObj from this command with TestData.tenantId prepended to all the
    // applicable database names and namespaces.
    const cmdObjWithTenantId = createCmdObjWithTenantId(cmdObj);

    let numAttempts = 0;

    while (true) {
        numAttempts++;
        let resObj = originalRunCommand.apply(
            this, [prependTenantIdToDbNameIfApplicable(dbName), cmdObjWithTenantId, options]);

        // Remove TestData.tenantId from all database names and namespaces in the resObj since tests
        // assume the command was run against the original database.
        removeTenantId(resObj);

        if (resObj.code != ErrorCodes.TenantMigrationAborted) {
            return resObj;
        }
        jsTest.log("Got TenantMigrationAborted after trying " + numAttempts +
                   " times, retrying command " + tojson(cmdObj));
    }
};

Mongo.prototype.runCommandWithMetadata = function(dbName, metadata, commandArgs) {
    // Create another cmdObj from this command with TestData.tenantId prepended to all the
    // applicable database names and namespaces.
    const commandArgsWithTenantId = createCmdObjWithTenantId(commandArgs);

    let numAttempts = 0;

    while (true) {
        numAttempts++;
        let resObj = originalRunCommand.apply(
            this, [prependTenantIdToDbNameIfApplicable(dbName), metadata, commandArgsWithTenantId]);

        // Remove TestData.tenantId from all database names and namespaces in the resObj since tests
        // assume the command was run against the original database.
        removeTenantId(resObj);

        if (resObj.code != ErrorCodes.TenantMigrationAborted) {
            return resObj;
        }
        jsTest.log("Got TenantMigrationAborted after trying " + numAttempts +
                   " times, retrying command " + tojson(commandArgs));
    }
};

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/inject_tenant_prefix.js");
}());
