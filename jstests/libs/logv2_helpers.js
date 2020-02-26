// logv2_helper.js

function isJsonLogNoConn() {
    return true;
}

function isJsonLog(conn) {
    const buildInfo = assert.commandWorked(conn.getDB("admin").runCommand({"buildinfo": 1}));
    if (buildInfo.version.length >= 3 && buildInfo.version.substring(0, 3) == "4.2") {
        return false;
    }

    return true;
}
