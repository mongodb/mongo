// Ensures the hostInfo command returns complete and consistent information. Does not evaluate
// correctness of the information.
// @tags: [
//   # The test runs commands that are not allowed with security token: hostInfo.
//   not_allowed_with_signed_security_token,
// ]
// SERVER-4615:  Ensure hostInfo() command returns expected results on each platform

(function() {
'use strict';

function assertNoneOf(expr, values, msg) {
    for (const value of values) {
        // Use strict equality check so that we correctly evaluate `false` as distinct from `null`.
        assert(expr !== value,
               tojson(expr) + " should be none of " + tojson(values) + ", but is equivalent to " +
                   tojson(value) + " : " + msg);
    }
}

const hostinfo = assert.commandWorked(db.hostInfo());

// test for os-specific fields
if (hostinfo.os.type == "Windows") {
    assertNoneOf(hostinfo.os.name, ["", null], "Missing Windows os name");
    assertNoneOf(hostinfo.os.version, ["", null], "Missing Windows version");

} else if (hostinfo.os.type == "Linux") {
    assertNoneOf(hostinfo.os.name, ["", null], "Missing Linux os/distro name");
    assertNoneOf(hostinfo.os.version, ["", null], "Missing Linux version");

} else if (hostinfo.os.type == "Darwin") {
    assertNoneOf(hostinfo.os.name, ["", null], "Missing Darwin os name");
    assertNoneOf(hostinfo.os.version, ["", null], "Missing Darwin version");

} else if (hostinfo.os.type == "BSD") {
    assertNoneOf(hostinfo.os.name, ["", null], "Missing FreeBSD os name");
    assertNoneOf(hostinfo.os.version, ["", null], "Missing FreeBSD version");
}

jsTest.log(hostinfo);
// comment out this block for systems which have not implemented hostinfo.
if (hostinfo.os.type != "") {
    assertNoneOf(hostinfo.system.hostname, ["", null], "Missing Hostname");
    assertNoneOf(hostinfo.system.currentTime, ["", null], "Missing Current Time");
    assertNoneOf(hostinfo.system.cpuAddrSize, ["", null, 0], "Missing CPU Address Size");
    assertNoneOf(hostinfo.system.memSizeMB, ["", null], "Missing Memory Size");
    assertNoneOf(hostinfo.system.numCores, ["", null, 0], "Missing Number of Logical Cores");
    // Check that numCoresAvailableToProcess != -1 as that indicates syscall failure.
    assertNoneOf(hostinfo.system.numCoresAvailableToProcess,
                 ["", null, -1],
                 "Missing Number of Cores Available To Process");
    assertNoneOf(
        hostinfo.system.numPhysicalCores, ["", null, 0], "Missing Number of Physical Cores");
    assertNoneOf(hostinfo.system.numCpuSockets, ["", null, 0], "Missing Number of CPU Sockets");
    assertNoneOf(hostinfo.system.cpuArch, ["", null], "Missing CPU Architecture");
    assertNoneOf(hostinfo.system.numaEnabled, ["", null], "Missing NUMA flag");
    assertNoneOf(hostinfo.system.numNumaNodes, ["", null, 0], "Missing Number of NUMA Nodes");
}

const buildInfo = assert.commandWorked(db.runCommand({buildInfo: 1}));
if (buildInfo.buildEnvironment && buildInfo.buildEnvironment.target_arch) {
    const targetArch = buildInfo.buildEnvironment.target_arch;
    if (targetArch == "i386")
        assert.eq(hostinfo.system.cpuAddrSize, 32);
    else
        assert.eq(hostinfo.system.cpuAddrSize, 64);
    assert.eq(hostinfo.system.cpuAddrSize, buildInfo.bits);
}
})();
