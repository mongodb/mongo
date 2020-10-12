// @tags: [
//     # `hostInfo` command is not available on embedded
//     incompatible_with_embedded,
// ]
// SERVER-4615:  Ensure hostInfo() command returns expected results on each platform
//
(function() {
    'use strict';

    function commonOSAsserts(hostinfo) {
        assert(hostinfo.os.hasOwnProperty('name'), "Missing " + hostinfo.os.type + " os name");
        assert(hostinfo.os.hasOwnProperty('version'), "Missing " + hostinfo.os.type + " version");
    }

    function coresAsserts(hostinfo) {
        assert.gt(
            hostinfo.extra.physicalCores, 0, "Missing " + hostinfo.os.type + " physical cores");
        assert.gt(hostinfo.system.numCores, 0, "Missing " + hostinfo.os.type + " logical cores");
        assert.lte(hostinfo.extra.physicalCores,
                   hostinfo.system.numCores,
                   hostinfo.os.type + " physical cores not larger then logical cores");
    }

    assert.commandWorked(db.hostInfo());
    var hostinfo = db.hostInfo();

    // test for os-specific fields
    if (hostinfo.os.type == "Windows") {
        commonOSAsserts(hostinfo);
        coresAsserts(hostinfo);
    } else if (hostinfo.os.type == "Linux") {
        commonOSAsserts(hostinfo);
        coresAsserts(hostinfo);
    } else if (hostinfo.os.type == "Darwin") {
        commonOSAsserts(hostinfo);
        coresAsserts(hostinfo);
    } else if (hostinfo.os.type == "BSD") {
        commonOSAsserts(hostinfo);
    }

    if (hostinfo.os.type != "") {
        assert(hostinfo.system.hasOwnProperty('hostname'), "Missing Hostname");
        assert(hostinfo.system.hasOwnProperty('currentTime'), "Missing Current Time");
        assert(hostinfo.system.hasOwnProperty('cpuAddrSize'), "Missing CPU Address Size");
        assert(hostinfo.system.hasOwnProperty('memSizeMB'), "Missing Memory Size");
        assert(hostinfo.system.hasOwnProperty('numCores'), "Missing Number of Cores");
        assert(hostinfo.system.hasOwnProperty('cpuArch'), "Missing CPU Architecture");
        assert(hostinfo.system.hasOwnProperty('numaEnabled'), "Missing NUMA flag");
    }

    var buildInfo = assert.commandWorked(db.runCommand({buildInfo: 1}));
    if (buildInfo.buildEnvironment && buildInfo.buildEnvironment.target_arch) {
        let targetArch = buildInfo.buildEnvironment.target_arch;
        if (targetArch == "i386")
            assert.eq(hostinfo.system.cpuAddrSize, 32);
        else
            assert.eq(hostinfo.system.cpuAddrSize, 64);
        assert.eq(hostinfo.system.cpuAddrSize, buildInfo.bits);
    }
})();
