// Returns non-localhost ipaddr of host running the mongo shell process
export function get_ipaddr() {
    // set temp path, if it exists
    let path = "";
    try {
        path = TestData.tmpPath;
        if (typeof path == "undefined") {
            path = "";
        } else if (path.slice(-1) != "/") {
            // Terminate path with / if defined
            path += "/";
        }
    } catch (err) {}

    let ipFile = path + "ipaddr-" + Random.srand() + ".log";
    let windowsCmd = "ipconfig > " + ipFile;
    let unixCmd = "(/sbin/ifconfig || /usr/sbin/ip addr) | grep 'inet ' | grep -v '127.0.0.1' > " + ipFile;
    let ipAddr = null;
    let hostType = null;

    try {
        hostType = getBuildInfo().buildEnvironment.target_os;

        // os-specific methods
        if (hostType == "windows") {
            runProgram("cmd.exe", "/c", windowsCmd);
            ipAddr = cat(ipFile).match(/IPv4.*: (.*)/)[1];
        } else {
            runProgram("/bin/sh", "-c", unixCmd);
            ipAddr = cat(ipFile)
                .replace(/addr:/g, "")
                .match(/inet ([\d]+\.[\d]+\.[\d]+\.[\d]+)/)[1];
        }
    } finally {
        removeFile(ipFile);
    }
    return ipAddr;
}

let ipv6Interfaces = [];

function getIPv6Address(linkLocal, interfaceOnly) {
    for (let i = 0; i < ipv6Interfaces.length; i++) {
        let iface = ipv6Interfaces[i];
        let addrInfos = iface.addr_info;
        for (let j = 0; j < addrInfos.length; j++) {
            let addr = addrInfos[j];
            if (addr.family != "inet6") {
                continue;
            }
            // Case: linkLocal && !interfaceOnly
            if (linkLocal && !interfaceOnly) {
                if (addr.scope === "link" && addr.local.startsWith("fe80")) {
                    return addr.local + "%" + iface.ifname;
                }
            }
            // Case: linkLocal && !interfaceOnly
            else if (!linkLocal && !interfaceOnly) {
                if (addr.scope === "global" && !addr.local.startsWith("fe80")) {
                    return addr.local;
                }
            }
            // Case: linkLocal && interfaceOnly
            else if (linkLocal && interfaceOnly) {
                if (addr.scope === "link" && addr.local.startsWith("fe80")) {
                    return iface.ifname;
                }
            }
            // If interfaceOnly is true and linklocal is false, do nothing (undefined)
        }
    }
    return "";
}

function populateIPv6Interfaces(inputJson) {
    let cleaned = [];
    try {
        // Parse the JSON input
        let parsed = JSON.parse(inputJson);
        // Filter interfaces with required fields
        for (let i = 0; i < parsed.length; i++) {
            let iface = parsed[i];
            // Only keep interfaces with ifname and addr_info array
            if (!iface.ifname || !Array.isArray(iface.addr_info)) {
                continue;
            }

            // Filter addr_info entries for required fields
            let filteredAddrInfo = [];
            for (let j = 0; j < iface.addr_info.length; j++) {
                let addr = iface.addr_info[j];
                if (addr.family && addr.scope && addr.local) {
                    filteredAddrInfo.push({
                        family: addr.family,
                        scope: addr.scope,
                        local: addr.local,
                    });
                }
            }
            if (filteredAddrInfo.length > 0) {
                cleaned.push({
                    ifname: iface.ifname,
                    addr_info: filteredAddrInfo,
                });
            }
        }
    } catch (e) {
        return [];
    }
    // Assign to global variable
    return cleaned;
}

export function getIpv6addr(linkLocal, interfaceOnly) {
    let ipv6File = "ipv6addr-" + Random.srand() + ".log";
    let ipv6ErrFile = "ipv6addr-" + Random.srand() + ".err";
    let unixCmd = "ip -6 -json addr > " + ipv6File + " 2> " + ipv6ErrFile;
    let inputJson = null;
    let ipv6Addr = null;

    try {
        let status = runProgram("/bin/sh", "-c", unixCmd);
        if (status !== 0) {
            return ipv6Addr;
        }
        inputJson = cat(ipv6File);
    } catch (e) {
        return ipv6Addr;
    } finally {
        removeFile(ipv6File);
        removeFile(ipv6ErrFile);
    }

    if (inputJson == null) {
        return ipv6Addr;
    }

    ipv6Interfaces = populateIPv6Interfaces(inputJson);

    return getIPv6Address(linkLocal, interfaceOnly);
}
