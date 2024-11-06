/**
 * Overrides the runCommand method to prefix all databases and namespaces ("config", "admin",
 * "local" excluded) with a tenant prefix.
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {
    createCmdObjWithTenantId,
    getTenantIdForDatabase,
    isCmdObjWithTenantId,
    prependTenantIdToDbNameIfApplicable,
    removeTenantIdAndMaybeCheckPrefixes,
} from "jstests/serverless/libs/tenant_prefixing.js";

// Assert that some tenantIds are provided
assert(!!TestData.tenantId || (TestData.tenantIds && TestData.tenantIds.length > 0),
       "Missing required tenantId or tenantIds");

// Save references to the original methods in the IIFE's scope.
// This scoping allows the original methods to be called by the overrides below.
const originalRunCommand = Mongo.prototype.runCommand;

// Save a reference to the connection created at shell startup. This will be used as a proxy for
// multiple internal routing connections for the lifetime of the test execution. If there is no
// initial connection, then we will not perform connection routing when using this override.
const initialConn = (typeof db !== 'undefined') ? db.getMongo() : undefined;

/**
 * Asserts that the provided connection is an internal routing connection, not the top-level proxy
 * connection. The proxy connection also has an internal routing connection, so it is excluded from
 * this check.
 */
function assertRoutingConnection(conn) {
    if (conn !== initialConn) {
        assert.eq(null,
                  conn._internalRoutingConnection,
                  "Expected connection to have no internal routing connection.");
    }
}

/**
 * @returns The internal routing connection for a provided connection
 */
function getRoutingConnection(conn) {
    if (conn === initialConn && conn._internalRoutingConnection == null) {
        conn._internalRoutingConnection = conn;
    }

    // Since we are patching the prototype below, there must eventually be a "base case" for
    // determining which connection to run a method on. If the provided `conn` has no internal
    // routing connection, we assume that it _is_ the internal routing connection, and return
    // here.
    if (conn._internalRoutingConnection == null) {
        return conn;
    }

    // Sanity check ensuring we have not accidentally created an internal routing connection on an
    // internal routing connection.
    assertRoutingConnection(conn._internalRoutingConnection);
    return conn._internalRoutingConnection;
}

function toIndexSet(indexedDocs) {
    let set = new Set();
    if (indexedDocs) {
        for (let doc of indexedDocs) {
            set.add(doc.index);
        }
    }
    return set;
}

/**
 * Remove the indices for non-upsert writes that succeeded.
 */
function removeSuccessfulOpIndexesExceptForUpserted(resObj, indexMap, ordered) {
    // Optimization to only look through the indices in a set rather than in an array.
    let indexSetForUpserted = toIndexSet(resObj.upserted);
    let indexSetForWriteErrors = toIndexSet(resObj.writeErrors);

    for (let index in Object.keys(indexMap)) {
        if ((!indexSetForUpserted.has(parseInt(index)) &&
             !(ordered && resObj.writeErrors && (index > resObj.writeErrors[0].index)) &&
             !indexSetForWriteErrors.has(parseInt(index)))) {
            delete indexMap[index];
        }
    }
    return indexMap;
}

/**
 * Rewrites a server connection string (ex: rsName/host,host,host) to a URI that the shell can
 * connect to.
 */
function convertServerConnectionStringToURI(input) {
    const inputParts = input.split('/');
    return `mongodb://${inputParts[1]}/?replicaSet=${inputParts[0]}`;
}

/**
 * Executes 'cmdObjWithTenantId'
 * 'dbNameWithTenantId' is only used for logging.
 */
function runCommandWithTenantId(
    conn, securityToken, dbNameWithTenantId, cmdObjWithTenantId, options) {
    // 'indexMap' is a mapping from a write's index in the current cmdObj to its index in the
    // original cmdObj.
    let indexMap = {};
    if (cmdObjWithTenantId.documents) {
        for (let i = 0; i < cmdObjWithTenantId.documents.length; i++) {
            indexMap[i] = i;
        }
    }
    if (cmdObjWithTenantId.updates) {
        for (let i = 0; i < cmdObjWithTenantId.updates.length; i++) {
            indexMap[i] = i;
        }
    }
    if (cmdObjWithTenantId.deletes) {
        for (let i = 0; i < cmdObjWithTenantId.deletes.length; i++) {
            indexMap[i] = i;
        }
    }

    const newConn = getRoutingConnection(conn);
    if (securityToken) {
        newConn._setSecurityToken(securityToken);
    }

    return originalRunCommand.apply(newConn, [dbNameWithTenantId, cmdObjWithTenantId, options]);
}

Mongo.prototype.runCommand = function(dbName, cmdObj, options) {
    const useSecurityToken = !!TestData.useSecurityToken;
    const useResponsePrefixChecking = !!TestData.useResponsePrefixChecking;

    const tenantId = getTenantIdForDatabase(dbName);
    const dbNameWithTenantId = prependTenantIdToDbNameIfApplicable(dbName, tenantId);
    const securityToken = useSecurityToken
        ? _createTenantToken({tenant: ObjectId(tenantId), expectPrefix: true})
        : undefined;

    // If the command is already prefixed, just run it
    if (isCmdObjWithTenantId(cmdObj)) {
        return runCommandWithTenantId(this, securityToken, dbNameWithTenantId, cmdObj, options);
    }

    // Prepend a tenant prefix to all database names and namespaces, where applicable.
    const cmdObjWithTenantId = createCmdObjWithTenantId(cmdObj, tenantId);

    const resObj = runCommandWithTenantId(
        this, securityToken, dbNameWithTenantId, cmdObjWithTenantId, options);

    // Remove the tenant prefix from all database names and namespaces in the result since tests
    // assume the command was run against the original database.
    const cmdName = Object.keys(cmdObj)[0];
    let checkPrefixOptions = !useResponsePrefixChecking ? {} : {
        checkPrefix: true,
        expectPrefix: true,
        tenantId,
        dbName,
        cmdName,
        debugLog: "Failed to check tenant prefix in response : " + tojsononeline(resObj) +
            ". The request command obj is " + tojsononeline(cmdObjWithTenantId)
    };

    removeTenantIdAndMaybeCheckPrefixes(resObj, checkPrefixOptions);

    return resObj;
};

Mongo.prototype.getDbNameWithTenantPrefix = function(dbName) {
    const tenantId = getTenantIdForDatabase(dbName);
    return prependTenantIdToDbNameIfApplicable(dbName, tenantId);
};

// Override base methods on the Mongo prototype to try to proxy the call to the underlying
// internal routing connection, if one exists.
// NOTE: This list is derived from scripting/mozjs/mongo.cpp:62.
['auth',
 'cleanup',
 'close',
 'compact',
 'getAutoEncryptionOptions',
 'isAutoEncryptionEnabled',
 'cursorHandleFromId',
 'find',
 'generateDataKey',
 'getDataKeyCollection',
 'logout',
 'encrypt',
 'decrypt',
 'isReplicaSetConnection',
 '_markNodeAsFailed',
 'getMinWireVersion',
 'getMaxWireVersion',
 'isReplicaSetMember',
 'isMongos',
 'isTLS',
 'getApiParameters',
 '_startSession',
 '_refreshAccessToken',
 // Don't override this method, since it is never called directly in jstests. The expectation of is
 // that it will be run on the connection `Mongo.prototype.runCommand` chose.
 // '_runCommandImpl',
].forEach(methodName => {
    const $method = Mongo.prototype[methodName];
    Mongo.prototype[methodName] = function() {
        return $method.apply(getRoutingConnection(this), arguments);
    };
});

// The following methods are overridden so that the method applies to both
// the proxy connection and the underlying internal routing connection, if one exists.
['toggleAutoEncryption',
 'unsetAutoEncryption',
 'setAutoEncryption',
].forEach(methodName => {
    const $method = Mongo.prototype[methodName];
    Mongo.prototype[methodName] = function() {
        let rc = getRoutingConnection(this);
        if (rc !== this) {
            $method.apply(rc, arguments);
        }
        return $method.apply(this, arguments);
    };
});

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/simulate_atlas_proxy.js");
