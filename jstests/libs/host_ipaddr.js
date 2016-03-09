// Returns non-localhost ipaddr of host running the mongo shell process
function get_ipaddr() {
    // set temp path, if it exists
    var path = "";
    try {
        path = TestData.tmpPath;
        if (typeof path == "undefined") {
            path = "";
        } else if (path.slice(-1) != "/") {
            // Terminate path with / if defined
            path += "/";
        }
    } catch (err) {
    }

    var ipFile = path + "ipaddr.log";
    var windowsCmd = "ipconfig > " + ipFile;
    var unixCmd = "/sbin/ifconfig | grep inet | grep -v '127.0.0.1' > " + ipFile;
    var ipAddr = null;
    var hostType = null;

    try {
        hostType = getBuildInfo().buildEnvironment.target_os;

        // os-specific methods
        if (hostType == "windows") {
            runProgram('cmd.exe', '/c', windowsCmd);
            ipAddr = cat(ipFile).match(/IPv4.*: (.*)/)[1];
        } else {
            runProgram('bash', '-c', unixCmd);
            ipAddr = cat(ipFile).replace(/addr:/g, "").match(/inet (.[^ ]*) /)[1];
        }
    } finally {
        removeFile(ipFile);
    }
    return ipAddr;
}
