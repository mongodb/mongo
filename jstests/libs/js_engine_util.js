/**
 * Returns true if the running mongod uses the MozJS-WASM JavaScript engine.
 * @param {DB} [targetDb] - the database connection to query; falls back to the global `db` if
 *     omitted.
 */
export function isMozjsWasm(targetDb) {
    const db_ = targetDb !== undefined ? targetDb : db;
    return db_.adminCommand({buildInfo: 1}).javascriptEngine === "mozjs-wasm";
}

/**
 * Returns true if the server was compiled with ThreadSanitizer (TSAN).
 * @param {DB} targetDb - the database connection to query (e.g. st.s0.getDB("admin")).
 */
export function isTsan(targetDb) {
    return targetDb.getServerBuildInfo().isThreadSanitizerActive();
}

/**
 * Returns true if the connected server has a server-side JavaScript engine.
 *
 * Server-side JS is compiled out of some builds (e.g. ppc64le links scripting_none) and can be
 * turned off at runtime with --noscripting. In both cases the {features:1} command omits its
 * "js" subdocument, which is what this checks.
 */
export function isServerSideJavaScriptEnabled(conn) {
    return assert.commandWorked(conn.adminCommand({features: 1})).js !== undefined;
}
