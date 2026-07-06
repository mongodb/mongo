/**
 * Returns true if the running mongod uses the MozJS-WASM JavaScript engine.
 * @param {DB} targetDb - the database connection to query (e.g. st.s0.getDB("admin")).
 */
export function isMozjsWasm(targetDb) {
    return targetDb.adminCommand({buildInfo: 1}).javascriptEngine === "mozjs-wasm";
}

/**
 * Returns true if the server was compiled with ThreadSanitizer (TSAN).
 * @param {DB} targetDb - the database connection to query (e.g. st.s0.getDB("admin")).
 */
export function isTsan(targetDb) {
    return targetDb.getServerBuildInfo().isThreadSanitizerActive();
}
