var exitCodeSuccess = 0;
var exitCodeErr = 1;
// Go reserves exit code 2 for its own use.
var exitCodeBadOptions = 3;
var exitCodeStopped = 4;

// NOTE: On Windows, stopMongoProgramByPid doesn't terminiate a process in a
// way that it can control its exit code.
if (_isWindows()) {
  exitCodeStopped = exitCodeErr;
}

var rowRegex = /^sh\d+\|\s/;
// portRegex finds the port on a line which has enough whitespace-delimited
// values to be considered a stat line and not an error message
var portRegex = /^sh\d+\|\s+\S+:(\d+)(\s+\S+){16}/;

function statRows() {
  return rawMongoProgramOutput()
    .split("\n")
    .filter(function(r) {
      return r.match(rowRegex);
    })
    .map(function(r) {
      return r.replace(/^sh\d+\| /, "");
    });
}

function statFields(row) {
  return row.split(/\s/).filter(function(s) {
    return s !== "";
  });
}

function getLatestChunk() {
  var output = rawMongoProgramOutput();
  // mongostat outputs a blank line between each set of stats when there are
  // multiple hosts; we want just one chunk of stat lines
  var lineChunks = output.split("| \n");
  if (lineChunks.length === 1) {
    return lineChunks[0];
  }
  return lineChunks[lineChunks.length - 2];
}

function latestPortCounts() {
  var portCounts = {};
  getLatestChunk().split("\n").forEach(function(r) {
    var matches = r.match(portRegex);
    if (matches === null) {
      return;
    }
    var port = matches[1];
    if (!portCounts[port]) {
      portCounts[port] = 0;
    }
    portCounts[port]++;
  });
  return portCounts;
}

function hasPort(port) {
  port = String(port);
  return function() {
    return latestPortCounts()[port] >= 1;
  };
}

function lacksPort(port) {
  port = String(port);
  return function() {
    return latestPortCounts()[port] === undefined;
  };
}

function hasOnlyPorts(expectedPorts) {
  expectedPorts = expectedPorts.map(String);
  return function() {
    var portCounts = latestPortCounts();
    for (var port in portCounts) {
      if (expectedPorts.indexOf(port) === -1) {
        return false;
      }
    }
    for (var i in expectedPorts) {
      if (portCounts[expectedPorts[i]] !== 1) {
        return false;
      }
    }
    return true;
  };
}

function statCheck(args, checker) {
  clearRawMongoProgramOutput();
  pid = startMongoProgramNoConnect.apply(null, args);
  try {
    assert.soon(checker, "discoverTest wait timed out");
    return true;
  } catch (e) {
    return false;
  } finally {
    stopMongoProgramByPid(pid);
  }
}

function discoverTest(ports, connectHost) {
  return statCheck(["mongostat",
      "--host", connectHost,
      "--noheaders",
      "--discover"],
    hasOnlyPorts(ports));
}

