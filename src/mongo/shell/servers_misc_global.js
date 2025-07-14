// Populate global variables from modules for backwards compatibility

import {
    allocatePort,
    allocatePorts,
    resetAllocatedPorts,
    startParallelShell,
    testingReplication,
    ToolTest,
    uncheckedParallelShellPidsString,
} from "src/mongo/shell/servers_misc.js";

globalThis.ToolTest = ToolTest;
globalThis.allocatePort = allocatePort;
globalThis.allocatePorts = allocatePorts;
globalThis.resetAllocatedPorts = resetAllocatedPorts;
globalThis.startParallelShell = startParallelShell;
globalThis.testingReplication = testingReplication;
globalThis.uncheckedParallelShellPidsString = uncheckedParallelShellPidsString;
