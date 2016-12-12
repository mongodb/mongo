(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  load("jstests/libs/mongostat.js");

  var toolTest = getToolTest("stat_discover");
  var rs = new ReplSetTest({
    name: "rpls",
    nodes: 4,
    useHostName: true,
  });

  rs.startSet();
  rs.initiate();
  rs.awaitReplication();

  worked = statCheck(["mongostat",
      "--port", rs.liveNodes.master.port,
      "--discover"],
    hasOnlyPorts(rs.ports));
  assert(worked, "when only port is used, each host still only appears once");

  assert(discoverTest(rs.ports, rs.liveNodes.master.host), "--discover against a replset master sees all members");

  assert(discoverTest(rs.ports, rs.liveNodes.slaves[0].host), "--discover against a replset slave sees all members");

  hosts = [rs.liveNodes.master.host, rs.liveNodes.slaves[0].host, rs.liveNodes.slaves[1].host];
  ports = [rs.liveNodes.master.port, rs.liveNodes.slaves[0].port, rs.liveNodes.slaves[1].port];
  worked = statCheck(['mongostat',
      '--host', hosts.join(',')],
    hasOnlyPorts(ports));
  assert(worked, "replica set specifiers are correctly used");

  assert(discoverTest([toolTest.port], toolTest.m.host), "--discover against a stand alone-sees just the stand-alone");

  // Test discovery with nodes cutting in and out
  clearRawMongoProgramOutput();
  pid = startMongoProgramNoConnect("mongostat", "--host", rs.liveNodes.slaves[1].host, "--discover");

  assert.soon(hasPort(rs.liveNodes.slaves[0].port), "discovered host is seen");
  assert.soon(hasPort(rs.liveNodes.slaves[1].port), "specified host is seen");

  rs.stop(rs.liveNodes.slaves[0]);
  assert.soon(lacksPort(rs.liveNodes.slaves[0].port), "after discovered host is stopped, it is not seen");
  assert.soon(hasPort(rs.liveNodes.slaves[1].port), "after discovered host is stopped, specified host is still seen");

  rs.start(rs.liveNodes.slaves[0]);
  assert.soon(hasPort(rs.liveNodes.slaves[0].port), "after discovered is restarted, discovered host is seen again");
  assert.soon(hasPort(rs.liveNodes.slaves[1].port), "after discovered is restarted, specified host is still seen");

  rs.stop(rs.liveNodes.slaves[1]);
  assert.soon(lacksPort(rs.liveNodes.slaves[1].port), "after specified host is stopped, specified host is not seen");
  assert.soon(hasPort(rs.liveNodes.slaves[0].port), "after specified host is stopped, the discovered host is still seen");

  stopMongoProgramByPid(pid);

  rs.stopSet();
  toolTest.stop();
}());
