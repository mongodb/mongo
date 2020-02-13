// logv2_helper.js

function isJsonLogNoConn() {
    const jsonDefault =
        getBuildInfo().buildEnvironment.cppdefines.indexOf("MONGO_CONFIG_JSON_LOG_DEFAULT") >= 0;
    if (typeof TestData !== 'undefined' && typeof TestData.logFormat !== 'undefined') {
        return TestData["logFormat"] == "json" || (jsonDefault && TestData["logFormat"] != "text");
    }

    return jsonDefault;
}

function isJsonLog(conn) {
    const jsonDefault =
        getBuildInfo().buildEnvironment.cppdefines.indexOf("MONGO_CONFIG_JSON_LOG_DEFAULT") >= 0;

    if (typeof TestData !== 'undefined' && typeof TestData.logFormat !== 'undefined') {
        return TestData["logFormat"] == "json" || (jsonDefault && TestData["logFormat"] != "text");
    }

    const opts = assert.commandWorked(conn.getDB("admin").runCommand({"getCmdLineOpts": 1}));
    const parsed = opts["parsed"];
    if (parsed.hasOwnProperty("systemLog") && parsed["systemLog"].hasOwnProperty("logFormat")) {
        return parsed["systemLog"]["logFormat"] == "json" ||
            (jsonDefault && parsed["systemLog"]["logFormat"] != "text");
    }

    return jsonDefault;
}
