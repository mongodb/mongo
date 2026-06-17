/**
 * Construct a new Mongo instance, retrying multiple times in case of failure.
 * @param  {...any} args to be passed onto the Mongo constructor.
 * @returns New Mongo instance
 * @throws After maximum retries have exceeded.
 */
export default function newMongoWithRetry(...args) {
    const MAX_RETRIES = 10;
    let retryCount = 0;

    // If TestData.hookAppName is set, inject the appName into the connection string so connections
    // opened here are tagged and can be identified by the server.
    //
    // Inlined rather than calling the shared injectHookAppName() helper in
    // jstests/libs/hook_appname.js because newMongoWithRetry is serialized into Thread
    // workers that can't import that module. Keep in sync with it.
    if (
        typeof TestData !== "undefined" &&
        TestData.hookAppName &&
        args.length > 0 &&
        typeof args[0] === "string"
    ) {
        let host = args[0];
        const appName = encodeURIComponent(TestData.hookAppName);
        if (host.startsWith("mongodb://")) {
            // Already a mongodb:// URI.
            if (host.includes("?")) {
                host = host + "&appName=" + appName;
            } else {
                // MongoDB URI requires a slash before query params if no database is specified.
                const slash = host.endsWith("/") ? "" : "/";
                host = host + slash + "?appName=" + appName;
            }
        } else if (host.includes("/")) {
            // Replica set format: replSetName/host:port[,host:port,...]
            // Convert to: mongodb://host:port[,host:port,...]/?replicaSet=replSetName&appName=...
            const slashIdx = host.indexOf("/");
            const replSetName = host.substring(0, slashIdx);
            const hosts = host.substring(slashIdx + 1);
            host = "mongodb://" + hosts + "/?replicaSet=" + replSetName + "&appName=" + appName;
        } else {
            // Simple host:port format.
            host = "mongodb://" + host + "/?appName=" + appName;
        }
        args[0] = host;
    }

    while (true) {
        try {
            return globalThis.Mongo.apply(this, args);
        } catch (error) {
            if (++retryCount >= MAX_RETRIES) {
                throw error;
            }
        }
    }
}
