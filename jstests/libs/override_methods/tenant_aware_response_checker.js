
function getDbName(nss) {
    if (nss.length === 0 || !nss.includes(".")) {
        return nss;
    }
    return nss.split(".")[0];
}

function wordInString(str, word) {
    let escapeRegExp = word.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    let regexp = new RegExp('\\b' + escapeRegExp + '\\b', 'i');
    return regexp.test(str);
}

function checkExpectedDbNameInString(str, dbName, prefixedDbName, originalRes) {
    // System db names (admin, local and config) should never be tenant prefixed.
    if (dbName == "admin" || dbName == "local" || dbName == "config") {
        assert.eq(false,
                  wordInString(str, prefixedDbName),
                  `Response db name "${str}" does not match sent db name "${
                      dbName}". The response is "${originalRes}"`);
        return;
    }
    // Currently, we do not expect prefixed db name in db name field as we only test with
    // "featureFlagRequireTenantID: true".
    // TODO SERVER-70740: expect prefixed db name if "expectPrefix" option in request is true.
    assert.eq(false,
              wordInString(str, prefixedDbName),
              `Response db name "${str}" does not match sent db name "${
                  dbName}". The response is "${originalRes}"`);
}

function checkExpectedDbInErrorMsg(errMsg, dbName, prefixedDbName, originalRes) {
    // The db name in error message should always include tenant prefixed db name regardless how the
    // tenantId was received in the request.

    // If the dbName doesn't exist in the error message at all, there is no need to check that it's
    // prefixed.
    if (!wordInString(errMsg, dbName)) {
        return;
    }

    // Skip check system db names (admin, local and config) which could be tenant prefixed or not.
    if (dbName == "admin" || dbName == "local" || dbName == "config") {
        return;
    }

    // Do not check change stream NoMatchingDocument error which does not contain prefixed db name.
    if (errMsg.includes("Change stream was configured to require a post-image") ||
        errMsg.includes("Change stream was configured to require a pre-image")) {
        return;
    }

    assert.eq(true,
              errMsg.includes(prefixedDbName),
              `The db name in the errmsg does not contain expected tenant prefixed db name "${
                  prefixedDbName}". The response is "${originalRes}"`);
}

/**
 * Check all the db names in the response are expected.
 * @param {*} res response object.
 * @param {*} requestDbName the original db name requested by jstest.
 * @param {*} prefixedDbName the tenant prefixed db name expected by inject_dollar_tenant.js and
 *     inject_security_toiken.js.
 * @param {*} originalResForLogging the original response for logging.
 */
function assertExpectedDbNameInResponse(res, requestDbName, prefixedDbName, originalResForLogging) {
    if (requestDbName.length === 0) {
        return;
    }

    for (let k of Object.keys(res)) {
        let v = res[k];
        if (typeof v === "string") {
            if (k === "dbName" || k == "db" || k == "dropped") {
                checkExpectedDbNameInString(
                    v, requestDbName, prefixedDbName, originalResForLogging);
            } else if (k === "namespace" || k === "ns") {
                checkExpectedDbNameInString(
                    getDbName(v), requestDbName, prefixedDbName, originalResForLogging);
            } else if (k == "name") {
                checkExpectedDbNameInString(
                    v, requestDbName, prefixedDbName, originalResForLogging);
            } else if (k === "errmsg") {
                checkExpectedDbInErrorMsg(v, requestDbName, prefixedDbName, originalResForLogging);
            }
        } else if (Array.isArray(v)) {
            v.forEach((item) => {
                if (typeof item === "object" && item !== null)
                    assertExpectedDbNameInResponse(
                        item, requestDbName, prefixedDbName, originalResForLogging);
            });
        } else if (typeof v === "object" && v !== null && Object.keys(v).length > 0) {
            assertExpectedDbNameInResponse(v, requestDbName, prefixedDbName, originalResForLogging);
        }
    }
}

function updateDbNamesInResponse(res, requestDbName, prefixedDbName) {
    for (let k of Object.keys(res)) {
        let v = res[k];
        if (typeof v === "string") {
            // Replace prefixed db name with db name.
            if (v.includes(prefixedDbName)) {
                res[k] = v.replace(prefixedDbName, requestDbName);
            }
        } else if (Array.isArray(v)) {
            v.forEach((item) => {
                if (typeof item === "object" && item !== null)
                    updateDbNamesInResponse(item, requestDbName, prefixedDbName);
            });
        } else if (typeof v === "object" && v !== null && Object.keys(v).length > 0) {
            updateDbNamesInResponse(v, requestDbName, prefixedDbName);
        }
    }
}
