# VSCode Extension for Debugging JS in the Mongo Shell

Use VSCode's Debugger UI with resmoke's `--shellJSDebugMode` flag.

![example.png](./example.png)

## Install

Run the following to package and install the latest extension:

```bash
./src/mongo/shell/debugger/vscode/install.sh
```

You'll see something like the following upon completion:

```
 DONE  Packaged: /home/ubuntu/mongo/src/mongo/shell/debugger/vscode/mongo-shell-debugger-1.0.0.vsix (7 files, 8.88 KB)
Installing extensions on SSH: steve-mcclure-0ed.workstations.build.10gen.cc...
Extension 'mongo-shell-debugger-1.0.0.vsix' was successfully installed.
```

> **One-time setup: Launch Configuration**
>
> Add a "mongo-shell" type configuration in your `~/.vscode/launch.json` file:
>
> ```json
> {
>   "version": "0.1.0",
>   "configurations": [
>     {
>       "type": "mongo-shell",
>       "request": "attach",
>       "name": "Attach to MongoDB Shell",
>       "debugPort": 9229
>     }
>   ]
> }
> ```

## Usage

1. Open a .js test file in VSCode
2. Add a breakpoint next to the line number (a red dot)
3. Start the debugger. Either:
   - Press F5 while in a JS file to start the (VSCode) debug server, or
   - In the "Run and Debug" side bar, choose "Attach to MongoDB Shell" in the dropdown, and click the play button.
     > You should see the following in the "Debug Console" of VSCode:
   ```
   Debug server listening on port 9229
   Waiting for mongo shell to connect on port 9229...
   Use resmoke's --shellJSDebugMode flag when running a JS test file to stop on breakpoints.
   ```
4. Run resmoke with the `--shellJSDebugMode` flag to stop on the breakpoints.
5. Use VSCode's breakpoint UI to navigate (continue, inspect scope variables, etc).

## Architecture

### Overview

```
┌─────────────────┐
│   VSCode UI     │
└────────┬────────┘
         │ DAP
         │
┌─────────────────┐
│   session.js    │  (VSCode Extension)
└────────┬────────┘
         │ JSON/TCP
         │
      :9229
         │
         │
┌─────────────────┐
│   adapter.cpp   │  (MongoDB Shell)
└────────┬────────┘
         │ DAP Messages
         │
┌─────────────────┐
│  debugger.cpp   │
└────────┬────────┘
         │ SM Debugger API
         │
┌─────────────────┐
│  SpiderMonkey   │  (JS Execution)
└─────────────────┘
```

### Components

**Client (VSCode Extension)**

- `extension.js` - Registers the extension and its configuration with VSCode
- `adapter.js` - Main entrypoint for VSCode Debug Adapter
- `session.js` - DAP server, listens on TCP port, translates VSCode ↔ shell protocol

**Server (MongoDB Shell)**

- `adapter.h/cpp` - DAP message handler, TCP client, connects to session.js
- `debugger.h/cpp` - SpiderMonkey Debugger API wrapper, breakpoint management

### Message Flow

**Initialization**

1. VSCode starts → session.js creates TCP server on :9229
2. Shell starts with --shellJSDebugMode → adapter.cpp connects to :9229
3. Shells waits for a "handshake" from the adapter
4. session.js sends queued breakpoints via "setBreakpoints" request

**Breakpoint Hit**

1. JS execution hits breakpoint → SpiderMonkey calls `hit()` handler
2. `debugger.cpp` stores location, sends "stopped" event via adapter
3. adapter blocks execution
4. VSCode shows paused state, requests stackTrace
5. User clicks continue → adapter unpauses, execution resumes

### Protocol

Newline-delimited JSON over TCP, eg:

```json
{"type":"request","seq":1,"command":"setBreakpoints","arguments":{...}}
{"type":"response","seq":1,"success":true,"body":{...}}
{"type":"event","event":"stopped","body":{"reason":"breakpoint"}}
```

### SpiderMonkey Integration

**Two Compartments**

- Main compartment: runs user JS, is observed by Debugger
- Debugger compartment: owns Debugger instance, separate from debuggee

**Breakpoint Mechanism**

1. Pending breakpoints stored in `_pendingBreakpoints` map
2. `onNewScript` callback fires when scripts load
3. Apply breakpoints via `script.setBreakpoint(offset, {hit: handler})`
4. Handler stores location, invokes C++ pause logic

**Execution Control**

- Pause: `_paused` atomic flag + condition variable blocks C++ thread
- Continue: flag cleared, condition variable notified, execution resumes
- REPL: stdin thread accepts commands, evaluates via `frame.eval()` in paused context
