
// Parse "127.0.0.1:300" into {addr: "127.0.0.1", port: 300},
// and "127.0.0.1" into {addr: "127.0.0.1", port: undefined}
function parseHost (hostString) {
    var items = hostString.match(/(\d+.\d+.\d+.\d+)(:(\d+))?/)
    return {addr: items[1], port: parseInt(items[3])}
}


/* Network traffic shaping (packet dropping) to simulate network problems
 Currently works on BSD Unix and Mac OS X only (using ipfw).
 Requires sudo access.
 TODO: make it work on Linux too (using iptables). */

var nextRuleNum = 100  // this grows indefinitely but can't exceed 65534, so can't call routines below indefinitely
var portRuleNum = {}

// Cut network connection to local port by dropping packets using iptables
function cutNetwork (port) {
    portRuleNum[port] = nextRuleNum
    runProgram ('sudo', 'ipfw', 'add ' + nextRuleNum++ + ' deny tcp from any to any ' + port)
    runProgram ('sudo', 'ipfw', 'add ' + nextRuleNum++ + ' deny tcp from any ' + port + ' to any')
    //TODO: confirm it worked (since sudo may not work)
    runProgram ('sudo', 'ipfw', 'show')
}

// Restore network connection to local port by not dropping packets using iptables
function restoreNetwork (port) {
    var ruleNum = portRuleNum[port]
    if (ruleNum) {
        runProgram ('sudo', 'ipfw', 'delete ' + ruleNum++)
        runProgram ('sudo', 'ipfw', 'delete ' + ruleNum)
        delete portRuleNum[port]
    }
    //TODO: confirm it worked (since sudo may not work)
    runProgram ('sudo', 'ipfw', 'show')
}
