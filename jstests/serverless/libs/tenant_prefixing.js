import {TxnUtil} from "jstests/libs/txns/txn_util.js";

const kDenylistedDbNames = new Set(["config", "admin", "local"]);
function isDenylistedDb(dbName) {
    return kDenylistedDbNames.has(dbName);
}

const kCmdsNotExpectSameDbNameInResp = new Set([
    // Following commands return different database names. It is by design.
    "validateDBMetadata",
    "connectionStatus",
    "mapReduce",
    "mapreduce",
]);
function shouldSkipPrefixCheck(cmdName, obj) {
    return kCmdsNotExpectSameDbNameInResp.has(cmdName) || (obj instanceof DBRef) ||
        (obj instanceof DBPointer);
}

/**
 * @returns Whether we are currently running an operation with multiple tenants.
 */
export function usingMultipleTenants() {
    return !!TestData.tenantIds;
}

const kTenantPrefixMap = {};

export function getTenantIdForDatabase(dbName) {
    if (!kTenantPrefixMap[dbName]) {
        const tenantId = usingMultipleTenants()
            ? TestData.tenantIds[Math.floor(Math.random() * TestData.tenantIds.length)]
            : TestData.tenantId;
        kTenantPrefixMap[dbName] = tenantId;
    }

    return kTenantPrefixMap[dbName];
}

/**
 * If the database with the given name can be migrated, prepend a tenant prefix if one has not
 * already been applied.
 */
export function prependTenantIdToDbNameIfApplicable(dbName, tenantId) {
    if (dbName.length === 0) {
        // There are input validation tests that use invalid database names, those should be
        // ignored.
        return dbName;
    }

    if (extractOriginalDbName(dbName) !== dbName) {
        // dbName already has a tenantId prefix
        return dbName;
    }

    const prefix = `${kTenantPrefixMap[dbName] || tenantId}_`;
    return (isDenylistedDb(dbName) || dbName.startsWith(prefix)) ? dbName : `${prefix}${dbName}`;
}

/**
 * If the database for the given namespace can be migrated, prepend a tenant prefix if one has not
 * already been applied.
 */
function prependTenantIdToNsIfApplicable(ns, tenantId) {
    if (ns.length === 0 || !ns.includes(".")) {
        // There are input validation tests that use invalid namespaces, those should be ignored.
        return ns;
    }

    const splitNs = ns.split(".");
    splitNs[0] = prependTenantIdToDbNameIfApplicable(splitNs[0], tenantId);
    return splitNs.join(".");
}

/**
 * Remove a tenant prefix from the provided database name, if applicable.
 */
function extractOriginalDbName(dbName) {
    const anyTenantPrefixOnceRegex =
        new RegExp(`^(${Object.values(kTenantPrefixMap).map(tid => `${tid}_`).join('|')})`, '');
    return dbName.replace(anyTenantPrefixOnceRegex, "");
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
    const anyTenantPrefixGlobalRegex =
        new RegExp(Object.values(kTenantPrefixMap).map(tid => `${tid}_`).join('|'), 'g');
    return string.replace(anyTenantPrefixGlobalRegex, "");
}

/**
 * Prepends a tenant prefix to all database name and namespace fields in the provided object, where
 * applicable.
 */
function prependTenantId(obj, tenantId) {
    for (let k of Object.keys(obj)) {
        let v = obj[k];
        if (typeof v === "string") {
            if (k === "dbName" || k == "db") {
                obj[k] = prependTenantIdToDbNameIfApplicable(v, tenantId);
            } else if (k === "namespace" || k === "ns") {
                obj[k] = prependTenantIdToNsIfApplicable(v, tenantId);
            }
        } else if (Array.isArray(v)) {
            obj[k] = v.map((item) => {
                return (typeof item === "object" && item !== null) ? prependTenantId(item, tenantId)
                                                                   : item;
            });
        } else if (typeof v === "object" && v !== null && Object.keys(v).length > 0) {
            obj[k] = prependTenantId(v, tenantId);
        }
    }

    return obj;
}

/**
 * Check the db name in error message.
 * The db name and namesapce string in error message should always include tenant prefix
 * regardless how the tenantId was received in the request.
 */
export function checkDbInErrorMsg(errMsg, dbName) {
    // Skip check system db names (admin, local and config) which could be tenant prefixed or not.
    if (dbName == "admin" || dbName == "local" || dbName == "config") {
        return true;
    }

    let words = errMsg.split(/[ ,]+/);
    const findExactDbName = words.includes(dbName);
    if (findExactDbName) {
        return false;
    }

    // We expect ns starts with `<tenantId>_<dbName>_` instead of `<dbName>_`.
    let dbPrefix = dbName + "_";
    for (const word in words) {
        if (word.startsWith(dbPrefix)) {
            return false;
        }
    }
    return true;
}

/**
 * Check if the raw command response is a CollectionUUIDMismatch error.
 * CollectionUUIDMismatch includes tenant prefixed db name in "db" instead of "errmsg".
 * There are three different locations for error "code" and "db":
 * - they are children of root response object.
 * - they are children of "writeErrors" object.
 * - they are children of "writeConcernError" object.
 */
function isCollectionUUIDMismatch(res) {
    if (!res.hasOwnProperty("ok")) {
        return false;
    }

    const mismatchCode = ErrorCodes.CollectionUUIDMismatch;
    if (res.hasOwnProperty("code") && res.code == mismatchCode) {
        return true;
    }

    if (res.hasOwnProperty("writeConcernError") && res.writeConcernError.code == mismatchCode) {
        return true;
    }

    if (res.hasOwnProperty("writeErrors")) {
        for (let err of res.writeErrors) {
            if (err.code == mismatchCode) {
                return true;
            }
        }
    }
    return false;
}

/**
 * Remove tenant id from CollectionUUIDMismatch object.
 */
function removeTenantIdFromCollectionUUIDMismatch(obj, checkPrefix, tenantId) {
    const prefix = tenantId + "_";
    const debugLog =
        `The db name in the errmsg does not contain matched tenant prefix
                        "${prefix}". The response is "${tojsononeline(obj)}"`;

    let errors = [];
    const mismatchCode = ErrorCodes.CollectionUUIDMismatch;
    if (obj.hasOwnProperty("code") && obj.code == mismatchCode) {
        errors.push(obj);
    } else if (obj.hasOwnProperty("writeConcernError") &&
               obj.writeConcernError.code == mismatchCode) {
        errors.push(obj.writeConcernError);
    } else if (obj.hasOwnProperty("writeErrors")) {
        for (let err of obj.writeErrors) {
            if (err.code == mismatchCode) {
                errors.push(err);
            }
        }
    }

    for (let err of errors) {
        if (checkPrefix) {
            assert(err.db.startsWith(prefix), debugLog);
        }
        err.db = extractOriginalDbName(err.db);
    }
}

/**
 * Removes a tenant prefix from all the database name and namespace fields in the provided object,
 * where applicable. Optionally check that namespaces are prefixed with the expected tenant id.
 *
 * @param {object} obj The command object to remove prefixes from
 * @param {object} [options] Optional settings
 * @param {boolean} [options.checkPrefix] Enable prefix checking for namespace strings
 * @param {string} [options.tenantId] The tenant the command is run on behalf of
 * @param {string} [options.dbName] The database name the command is run against
 * @param {string} [options.cmdName] The command name
 * @param {string} [options.debugLog] The debug log for a failed prefix checking
 */
export function removeTenantIdAndMaybeCheckPrefixes(obj, options = {
    checkPrefix: false,
    expectPrefix: false,
    tenantId: undefined,
    dbName: undefined,
    cmdName: undefined,
    debugLog: undefined,
}) {
    const {checkPrefix, expectPrefix, tenantId, dbName: requestDbName, cmdName, debugLog} = options;
    if (checkPrefix) {
        assert(tenantId != null, "Missing required option `tenantId` when checking prefixes");
        assert(requestDbName != null, "Missing required option `dbName` when checking prefixes");
    }

    if (isCollectionUUIDMismatch(obj)) {
        removeTenantIdFromCollectionUUIDMismatch(obj, checkPrefix, tenantId);
        return;
    }

    const expectedDbName = expectPrefix ? `${tenantId}_${requestDbName}` : requestDbName;
    for (let k of Object.keys(obj)) {
        let v = obj[k];
        let originalK = removeTenantIdFromString(k);
        if (typeof v === "string") {
            if (k === "dbName" || k == "db" || k == "dropped") {
                if (checkPrefix && !isDenylistedDb(requestDbName) &&
                    !shouldSkipPrefixCheck(cmdName, obj)) {
                    assert.eq(v, expectedDbName, debugLog);
                }
                obj[originalK] = extractOriginalDbName(v);
            } else if (k === "namespace" || k === "ns") {
                if (checkPrefix) {
                    const responseDbName = v.split('.')[0];
                    if (!isDenylistedDb(requestDbName) && !shouldSkipPrefixCheck(cmdName, obj)) {
                        assert.eq(responseDbName, expectedDbName, debugLog);
                    }
                }

                obj[originalK] = extractOriginalNs(v);
            } else if (k === "errmsg") {
                if (checkPrefix) {
                    assert(checkDbInErrorMsg(v, requestDbName), debugLog);
                }
                obj[originalK] = removeTenantIdFromString(v);
            } else if (k == "name") {
                // TODO(): improve response checking
                obj[originalK] = removeTenantIdFromString(v);
            }
        } else if (Array.isArray(v)) {
            obj[originalK] = v.map((item) => {
                return (typeof item === "object" && item !== null)
                    ? removeTenantIdAndMaybeCheckPrefixes(item, options)
                    : item;
            });
        } else if (typeof v === "object" && v !== null && Object.keys(v).length > 0) {
            obj[originalK] = removeTenantIdAndMaybeCheckPrefixes(v, options);
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
export function createCmdObjWithTenantId(cmdObj, tenantId) {
    const cmdName = Object.keys(cmdObj)[0];
    let cmdObjWithTenantId = TxnUtil.deepCopyObject({}, cmdObj);

    if (isCmdObjWithTenantId(cmdObj)) {
        return cmdObjWithTenantId;
    }

    // Handle commands with special database and namespace field names.
    if (kCmdsWithNsAsFirstField.has(cmdName)) {
        cmdObjWithTenantId[cmdName] =
            prependTenantIdToNsIfApplicable(cmdObjWithTenantId[cmdName], tenantId);
    }

    switch (cmdName) {
        case "renameCollection":
            cmdObjWithTenantId.to =
                prependTenantIdToNsIfApplicable(cmdObjWithTenantId.to, tenantId);
            break;
        case "internalRenameIfOptionsAndIndexesMatch":
            cmdObjWithTenantId.from =
                prependTenantIdToNsIfApplicable(cmdObjWithTenantId.from, tenantId);
            cmdObjWithTenantId.to =
                prependTenantIdToNsIfApplicable(cmdObjWithTenantId.to, tenantId);
            break;
        case "configureFailPoint":
            if (cmdObjWithTenantId.data) {
                if (cmdObjWithTenantId.data.namespace) {
                    cmdObjWithTenantId.data.namespace = prependTenantIdToNsIfApplicable(
                        cmdObjWithTenantId.data.namespace, tenantId);
                } else if (cmdObjWithTenantId.data.ns) {
                    cmdObjWithTenantId.data.ns =
                        prependTenantIdToNsIfApplicable(cmdObjWithTenantId.data.ns, tenantId);
                }
            }
            break;
        case "applyOps":
            for (let op of cmdObjWithTenantId.applyOps) {
                if (typeof op.ns === "string" && op.ns.endsWith("system.views") && op.o._id &&
                    typeof op.o._id === "string") {
                    // For views, op.ns and op.o._id must be equal.
                    op.o._id = prependTenantIdToNsIfApplicable(op.o._id, tenantId);
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
        prependTenantId(cmdObjWithTenantId, tenantId);
    }

    cmdObjWithTenantId.comment = Object.assign(
        cmdObjWithTenantId.comment ? cmdObjWithTenantId.comment : {}, {isCmdObjWithTenantId: true});
    return cmdObjWithTenantId;
}
