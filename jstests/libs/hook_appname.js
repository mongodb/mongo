/**
 * Helper for tagging shell-created connections with an appName so that connections opened by
 * resmoke hooks can be identified by the server.
 *
 * Hooks that need their connections tagged set TestData.hookAppName. Connections opened from within
 * JavaScript (e.g. via `new Mongo(host)`) don't automatically carry that appName, so this helper
 * rewrites the connection string to include it.
 */

/**
 * If TestData.hookAppName is set, returns `host` with the appName injected into the connection
 * string. Otherwise (or if `host` is not a string) returns `host` unchanged.
 *
 * This logic is also inlined in jstests/libs/retryable_mongo.js (which can't import this module);
 * keep the two in sync.
 */
export function injectHookAppName(host) {
    if (typeof TestData === "undefined" || !TestData.hookAppName || typeof host !== "string") {
        return host;
    }

    const appName = encodeURIComponent(TestData.hookAppName);

    if (host.startsWith("mongodb://")) {
        // Already a mongodb:// URI.
        if (host.includes("?")) {
            return host + "&appName=" + appName;
        }
        // MongoDB URI requires a slash before query params if no database is specified.
        const slash = host.endsWith("/") ? "" : "/";
        return host + slash + "?appName=" + appName;
    }

    if (host.includes("/")) {
        // Replica set format: replSetName/host:port[,host:port,...]
        // Convert to: mongodb://host:port[,host:port,...]/?replicaSet=replSetName&appName=...
        const slashIdx = host.indexOf("/");
        const replSetName = host.substring(0, slashIdx);
        const hosts = host.substring(slashIdx + 1);
        return "mongodb://" + hosts + "/?replicaSet=" + replSetName + "&appName=" + appName;
    }

    // Simple host:port format.
    return "mongodb://" + host + "/?appName=" + appName;
}
