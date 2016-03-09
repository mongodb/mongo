// SERVER-4615:  Ensure hostInfo() command returns expected results on each platform

assert.commandWorked(db.hostInfo());
var hostinfo = db.hostInfo();

// test for os-specific fields
if (hostinfo.os.type == "Windows") {
    assert.neq(hostinfo.os.name, "" || null, "Missing Windows os name");
    assert.neq(hostinfo.os.version, "" || null, "Missing Windows version");

} else if (hostinfo.os.type == "Linux") {
    assert.neq(hostinfo.os.name, "" || null, "Missing Linux os/distro name");
    assert.neq(hostinfo.os.version, "" || null, "Missing Lindows version");

} else if (hostinfo.os.type == "Darwin") {
    assert.neq(hostinfo.os.name, "" || null, "Missing Darwin os name");
    assert.neq(hostinfo.os.version, "" || null, "Missing Darwin version");

} else if (hostinfo.os.type == "BSD") {
    assert.neq(hostinfo.os.name, "" || null, "Missing FreeBSD os name");
    assert.neq(hostinfo.os.version, "" || null, "Missing FreeBSD version");
}

// comment out this block for systems which have not implemented hostinfo.
if (hostinfo.os.type != "") {
    assert.neq(hostinfo.system.hostname, "" || null, "Missing Hostname");
    assert.neq(hostinfo.system.currentTime, "" || null, "Missing Current Time");
    assert.neq(hostinfo.system.cpuAddrSize, "" || null || 0, "Missing CPU Address Size");
    assert.neq(hostinfo.system.memSizeMB, "" || null, "Missing Memory Size");
    assert.neq(hostinfo.system.numCores, "" || null || 0, "Missing Number of Cores");
    assert.neq(hostinfo.system.cpuArch, "" || null, "Missing CPU Architecture");
    assert.neq(hostinfo.system.numaEnabled, "" || null, "Missing NUMA flag");
}
