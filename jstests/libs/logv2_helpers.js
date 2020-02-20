// logv2_helper.js

function isJsonLogNoConn() {
    const textDefault =
        getBuildInfo().buildEnvironment.cppdefines.indexOf("MONGO_CONFIG_TEXT_LOG_DEFAULT") >= 0;
    if (typeof TestData !== 'undefined' && typeof TestData.logFormat !== 'undefined' &&
        TestData["logFormat"] != "") {
        return TestData["logFormat"] != "text" || (textDefault && TestData["logFormat"] == "json");
    }

    return !textDefault;
}

function isJsonLog(conn) {
    const textDefault =
        getBuildInfo().buildEnvironment.cppdefines.indexOf("MONGO_CONFIG_TEXT_LOG_DEFAULT") >= 0;
    if (typeof TestData !== 'undefined' && typeof TestData.logFormat !== 'undefined' &&
        TestData["logFormat"] != "") {
        return TestData["logFormat"] != "text" || (textDefault && TestData["logFormat"] == "json");
    }

    const opts = assert.commandWorked(conn.getDB("admin").runCommand({"getCmdLineOpts": 1}));
    const parsed = opts["parsed"];
    if (parsed.hasOwnProperty("systemLog") && parsed["systemLog"].hasOwnProperty("logFormat") &&
        parsed["systemLog"]["logFormat"] != "") {
        return parsed["systemLog"]["logFormat"] != "text" ||
            (textDefault && parsed["systemLog"]["logFormat"] == "json");
    }

    const buildInfo = assert.commandWorked(conn.getDB("admin").runCommand({"buildinfo": 1}));
    if (buildInfo.version.length >= 3 && buildInfo.version.substring(0, 3) == "4.2") {
        return false;
    }

    return !textDefault;
}
