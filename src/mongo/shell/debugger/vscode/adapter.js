#!/usr/bin/env node

/**
 * Debug Adapter entry point for MongoDB Shell JS Debugger
 * This executable is invoked by VSCode when starting a debug session
 */

console.log("Starting MongoDB Shell JS Debug Adapter...");

const {MongoShellDebugSession} = require("./session");
const {DebugSession} = require("@vscode/debugadapter");

// Start the debug session
DebugSession.run(MongoShellDebugSession);
