// logv2_helper.js

function isJsonLogNoConn() {
    if (typeof TestData !== 'undefined' && typeof TestData.logFormat !== 'undefined') {
        return TestData["logFormat"] == "json";
    }

    return false;
}

function isJsonLog(conn) {
    if (typeof TestData !== 'undefined' && typeof TestData.logFormat !== 'undefined') {
        return TestData["logFormat"] == "json";
    }

    const opts = assert.commandWorked(conn.getDB("admin").runCommand({"getCmdLineOpts": 1}));

    print(tojson(opts));

    const parsed = opts["parsed"];
    if (parsed.hasOwnProperty("systemLog") && parsed["systemLog"].hasOwnProperty("logFormat")) {
        return parsed["systemLog"]["logFormat"] == "json";
    }

    return false;
}
