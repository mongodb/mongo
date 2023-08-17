import {TransactionsUtil} from "jstests/libs/transactions_util.js";

const kDenylistedDbNames = new Set(["config", "admin", "local"]);
function isDenylistedDb(dbName) {
    return kDenylistedDbNames.has(dbName);
}

/**
 * @returns Whether we are currently running an operation with multiple tenants.
 */
export function usingMultipleTenants() {
    return !!TestData.tenantIds;
}

const kTenantPrefixMap = {};

/**
 * If the database with the given name can be migrated, prepend a tenant prefix if one has not
 * already been applied.
 */
export function prependTenantIdToDbNameIfApplicable(dbName) {
    if (dbName.length === 0) {
        // There are input validation tests that use invalid database names, those should be
        // ignored.
        return dbName;
    }

    if (extractOriginalDbName(dbName) !== dbName) {
        // dbName already has a tenantId prefix
        return dbName;
    }

    let prefix;
    // If running shard split passthroughs, then assign a database to a randomly selected tenant
    if (usingMultipleTenants()) {
        if (!kTenantPrefixMap[dbName]) {
            const tenantId =
                TestData.tenantIds[Math.floor(Math.random() * TestData.tenantIds.length)];
            kTenantPrefixMap[dbName] = `${tenantId}_`;
        }

        prefix = kTenantPrefixMap[dbName];
    } else {
        prefix = `${TestData.tenantId}_`;
    }

    return (isDenylistedDb(dbName) || dbName.startsWith(prefix)) ? dbName : `${prefix}${dbName}`;
}

/**
 * If the database for the given namespace can be migrated, prepend a tenant prefix if one has not
 * already been applied.
 */
function prependTenantIdToNsIfApplicable(ns) {
    if (ns.length === 0 || !ns.includes(".")) {
        // There are input validation tests that use invalid namespaces, those should be ignored.
        return ns;
    }

    const splitNs = ns.split(".");
    splitNs[0] = prependTenantIdToDbNameIfApplicable(splitNs[0]);
    return splitNs.join(".");
}

/**
 * Remove a tenant prefix from the provided database name, if applicable.
 */
function extractOriginalDbName(dbName) {
    if (usingMultipleTenants()) {
        const anyTenantPrefixOnceRegex = new RegExp(Object.values(kTenantPrefixMap).join('|'), '');
        return dbName.replace(anyTenantPrefixOnceRegex, "");
    }

    return dbName.replace(`${TestData.tenantId}_`, "");
}

/**
 * Remove a tenant prefix from the provided namespace, if applicable.
 */
function extractOriginalNs(ns) {
    const splitNs = ns.split(".");
    splitNs[0] = extractOriginalDbName(splitNs[0]);
    return splitNs.join(".");
}

/**
 * Removes all occurrences of a tenant prefix in the provided string.
 */
function removeTenantIdFromString(string) {
    if (usingMultipleTenants()) {
        const anyTenantPrefixGlobalRegex =
            new RegExp(Object.values(kTenantPrefixMap).join('|'), 'g');
        return string.replace(anyTenantPrefixGlobalRegex, "");
    }

    return string.replace(new RegExp(`${TestData.tenantId}_`, 'g'), "");
}

/**
 * Prepends a tenant prefix to all database name and namespace fields in the provided object, where
 * applicable.
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
 * Removes a tenant prefix from all the database name and namespace fields in the provided object,
 * where applicable.
 */
export function removeTenantId(obj) {
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
 * Returns true if the provided command object has had a tenant prefix appended to its namespaces.
 */
export function isCmdObjWithTenantId(cmdObj) {
    return cmdObj.comment && cmdObj.comment.isCmdObjWithTenantId;
}

/**
 * Prepend a tenant prefix to all namespaces within a provided command object, and record a comment
 * indicating that the command object has alrady been modified.
 */
export function createCmdObjWithTenantId(cmdObj) {
    const cmdName = Object.keys(cmdObj)[0];
    let cmdObjWithTenantId = TransactionsUtil.deepCopyObject({}, cmdObj);

    if (isCmdObjWithTenantId(cmdObj)) {
        return cmdObjWithTenantId;
    }

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

    cmdObjWithTenantId.comment = Object.assign(
        cmdObjWithTenantId.comment ? cmdObjWithTenantId.comment : {}, {isCmdObjWithTenantId: true});
    return cmdObjWithTenantId;
}
