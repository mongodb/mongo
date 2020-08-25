/**
 * Overrides the database name of each accessed database ("config", "admin", "local" excluded) to
 * have the prefix TestData.dbPrefix so that the accessed data will be migrated by the background
 * tenant migrations run by the ContinuousTenantMigration hook.
 */
(function() {
'use strict';

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.
load("jstests/libs/transactions_util.js");

// Save references to the original methods in the IIFE's scope.
// This scoping allows the original methods to be called by the overrides below.
// Override this method to make the accessed database have the prefix TestData.dbPrefix.
let originalRunCommand = Mongo.prototype.runCommand;

const blacklistedDbNames = ["config", "admin", "local"];

function isBlacklistedDb(dbName) {
    return blacklistedDbNames.includes(dbName);
}

/**
 * If the database with the given name can be migrated, prepends TestData.dbPrefix to the name if
 * it does not already start with the prefix.
 */
function prependDbPrefixToDbNameIfApplicable(dbName) {
    if (dbName.length === 0) {
        // There are input validation tests that use invalid database names, those should be
        // ignored.
        return dbName;
    }
    return isBlacklistedDb(dbName) ? dbName : TestData.dbPrefix + dbName;
}

/**
 * If the database for the given namespace can be migrated, prepends TestData.dbPrefix to the
 * namespace if it does not already start with the prefix.
 */
function prependDbPrefixToNsIfApplicable(ns) {
    if (ns.length === 0 || !ns.includes(".")) {
        // There are input validation tests that use invalid namespaces, those should be ignored.
        return ns;
    }
    let splitNs = ns.split(".");
    splitNs[0] = prependDbPrefixToDbNameIfApplicable(splitNs[0]);
    return splitNs.join(".");
}

/**
 * If the given database name starts TestData.dbPrefix, removes the prefix.
 */
function extractOriginalDbName(dbName) {
    return dbName.replace(TestData.dbPrefix, "");
}

/**
 * If the database name for the given namespace starts TestData.dbPrefix, removes the prefix.
 */
function extractOriginalNs(ns) {
    let splitNs = ns.split(".");
    splitNs[0] = extractOriginalDbName(splitNs[0]);
    return splitNs.join(".");
}

/**
 * Removes all occurrences of TestDatabase.dbPrefix in the string.
 */
function removeDbPrefixFromString(string) {
    return string.replace(new RegExp(TestData.dbPrefix, "g"), "");
}

/**
 * Prepends TestDatabase.dbPrefix to all the database name and namespace fields inside the given
 * object.
 */
function prependDbPrefix(obj) {
    for (let k of Object.keys(obj)) {
        let v = obj[k];
        if (typeof v === "string") {
            if (k === "dbName" || k == "db") {
                obj[k] = prependDbPrefixToDbNameIfApplicable(v);
            } else if (k === "namespace" || k === "ns") {
                obj[k] = prependDbPrefixToNsIfApplicable(v);
            }
        } else if (Array.isArray(v)) {
            obj[k] = v.map((item) => {
                return (typeof item === "object" && item !== null) ? prependDbPrefix(item) : item;
            });
        } else if (typeof v === "object" && v !== null && Object.keys(v).length > 0) {
            obj[k] = prependDbPrefix(v);
        }
    }
    return obj;
}

/**
 * Removes TestDatabase.dbPrefix from all the database name and namespace fields inside the given
 * object.
 */
function removeDbPrefix(obj) {
    for (let k of Object.keys(obj)) {
        let v = obj[k];
        let originalK = removeDbPrefixFromString(k);
        if (typeof v === "string") {
            if (k === "dbName" || k == "db" || k == "dropped") {
                obj[originalK] = extractOriginalDbName(v);
            } else if (k === "namespace" || k === "ns") {
                obj[originalK] = extractOriginalNs(v);
            } else if (k === "errmsg" || k == "name") {
                obj[originalK] = removeDbPrefixFromString(v);
            }
        } else if (Array.isArray(v)) {
            obj[originalK] = v.map((item) => {
                return (typeof item === "object" && item !== null) ? removeDbPrefix(item) : item;
            });
        } else if (typeof v === "object" && v !== null && Object.keys(v).length > 0) {
            obj[originalK] = removeDbPrefix(v);
        }
    }
    return obj;
}

const kCmdsWithNsAsFirstField =
    new Set(["renameCollection", "checkShardingIndex", "dataSize", "datasize", "splitVector"]);

/**
 * Returns a new cmdObj with TestData.dbPrefix prepended to all database name and namespace fields.
 */
function createCmdObjWithDbPrefix(cmdObj) {
    const cmdName = Object.keys(cmdObj)[0];
    let cmdObjWithDbPrefix = TransactionsUtil.deepCopyObject({}, cmdObj);

    // Handle commands with special database and namespace field names.
    if (kCmdsWithNsAsFirstField.has(cmdName)) {
        cmdObjWithDbPrefix[cmdName] = prependDbPrefixToNsIfApplicable(cmdObjWithDbPrefix[cmdName]);
    }

    switch (cmdName) {
        case "renameCollection":
            cmdObjWithDbPrefix.to = prependDbPrefixToNsIfApplicable(cmdObjWithDbPrefix.to);
            break;
        case "internalRenameIfOptionsAndIndexesMatch":
            cmdObjWithDbPrefix.from = prependDbPrefixToNsIfApplicable(cmdObjWithDbPrefix.from);
            cmdObjWithDbPrefix.to = prependDbPrefixToNsIfApplicable(cmdObjWithDbPrefix.to);
            break;
        case "configureFailPoint":
            if (cmdObjWithDbPrefix.data) {
                if (cmdObjWithDbPrefix.data.namespace) {
                    cmdObjWithDbPrefix.data.namespace =
                        prependDbPrefixToNsIfApplicable(cmdObjWithDbPrefix.data.namespace);
                } else if (cmdObjWithDbPrefix.data.ns) {
                    cmdObjWithDbPrefix.data.ns =
                        prependDbPrefixToNsIfApplicable(cmdObjWithDbPrefix.data.ns);
                }
            }
            break;
        case "applyOps":
            for (let op of cmdObjWithDbPrefix.applyOps) {
                if (typeof op.ns === "string" && op.ns.endsWith("system.views") && op.o._id &&
                    typeof op.o._id === "string") {
                    // For views, op.ns and op.o._id must be equal.
                    op.o._id = prependDbPrefixToNsIfApplicable(op.o._id);
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
        prependDbPrefix(cmdObjWithDbPrefix);
    }

    return cmdObjWithDbPrefix;
}

Mongo.prototype.runCommand = function(dbName, cmdObj, options) {
    // Create another cmdObj from this command with TestData.dbPrefix prepended to all the
    // applicable database names and namespaces.
    const cmdObjWithDbPrefix = createCmdObjWithDbPrefix(cmdObj);

    let resObj = originalRunCommand.apply(
        this, [prependDbPrefixToDbNameIfApplicable(dbName), cmdObjWithDbPrefix, options]);

    // Remove TestData.dbPrefix from all database names and namespaces in the resObj since tests
    // assume the command was run against the original database.
    removeDbPrefix(resObj);

    return resObj;
};

Mongo.prototype.runCommandWithMetadata = function(dbName, metadata, commandArgs) {
    // Create another cmdObj from this command with TestData.dbPrefix prepended to all the
    // applicable database names and namespaces.
    const commandArgsWithDbPrefix = createCmdObjWithDbPrefix(commandArgs);

    let resObj = originalRunCommand.apply(
        this, [prependDbPrefixToDbNameIfApplicable(dbName), metadata, commandArgsWithDbPrefix]);

    // Remove TestData.dbPrefix from all database names and namespaces in the resObj since tests
    // assume the command was run against the original database.
    removeDbPrefix(resObj);

    return resObj;
};

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/inject_tenant_prefix.js");
}());
