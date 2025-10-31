# Network Fault Injection Framework (mongobridge)

## Overview

[Mongobridge](https://github.com/mongodb/mongo/blob/e810af1916caaedb1cde8d1e1b74bb50b2461daf/src/mongo/tools/mongobridge_tool/bridge.cpp#L1) is a network fault injection testing tool that allows test authors to intentionally simulate network issues such as connection failures, message delays, or packet loss during communication to any node in a cluster. It acts as a transparent proxy between MongoDB processes and their clients, enabling controlled network fault injection for testing distributed system behavior.

## How It Works

When `ReplSetTest` or `ShardingTest` are instructed to use `mongobridge`, they will [set up a mongobridge process](https://github.com/mongodb/mongo/blob/e810af1916caaedb1cde8d1e1b74bb50b2461daf/jstests/libs/replsettest.js#L2962) for each node that [creates a ProxiedConnection](https://github.com/mongodb/mongo/blob/e810af1916caaedb1cde8d1e1b74bb50b2461daf/src/mongo/tools/mongobridge_tool/bridge.cpp#L323-L324) between the node and any clients (including other nodes in the cluster) attempting to communicate with it. When test authors send a command to a node, mongobridge [intercepts the command and applies any configured actions](https://github.com/mongodb/mongo/blob/e810af1916caaedb1cde8d1e1b74bb50b2461daf/src/mongo/tools/mongobridge_tool/bridge.cpp#L395-L430) onto the commands before forwarding the command along to the node itself. This allows simple fault injection from the test author's perspective.

## Quick Start

To use mongobridge in your tests:

1. **Enable mongobridge** in your test setup:

   ```javascript
   let st = new ShardingTest({
     shards: {rs0: {nodes: 2}},
     mongos: 1,
     config: 1,
     useBridge: true, // Enable mongobridge
   });
   ```

   - **Test commands must be enabled**: Mongobridge's `*From` commands require `enableTestCommands: true` (which is the default in test environments)

2. **Inject network faults** using bridge commands:

   ```javascript
   // Delay messages by 5 seconds
   st.rs0.getPrimary().delayMessagesFrom(st.rs0.getSecondary(), 5000);

   // Reject all connections
   st.rs0.getPrimary().rejectConnectionsFrom(st.rs0.getSecondary());

   // Restore normal behavior
   st.rs0.getPrimary().acceptConnectionsFrom(st.rs0.getSecondary());
   ```

3. Operations that depend on communication between the affected nodes will fail or timeout as expected.

## What to keep in mind

Be aware that there are consequences to injecting network faults between nodes that can cause downstream impact in (for example) heartbeats, sync source selection, and SDAM, and so after a fault has been injected the test may not be in the state you expect it to be in for future commands. It is best to keep mongobridge tests relatively short and targeted to ensure that flakiness due to these faults doesn't impact the rest of your testing.

## Command Reference

Mongobridge supports four commands for network fault injection:

### `acceptConnectionsFrom(bridges)`

**Purpose**: Allows normal communication from specified sources

**Usage**:

```javascript
node.acceptConnectionsFrom(otherNode);
node.acceptConnectionsFrom([node1, node2, node3]); // Multiple nodes
```

**Effect**: Restores normal message forwarding (default state)

### `rejectConnectionsFrom(bridges)`

**Purpose**: Immediately closes connections from specified sources

**Usage**:

```javascript
node.rejectConnectionsFrom(otherNode);
```

**Effect**: New connections are rejected, existing connections are closed when a new request is sent over them

**Use case**: Simulating complete network partitions

### `delayMessagesFrom(bridges, delayMs)`

**Purpose**: Delays message forwarding by specified milliseconds

**Usage**:

```javascript
node.delayMessagesFrom(otherNode, 5000); // 5 second delay
node.delayMessagesFrom(otherNode, 0); // Remove delay
```

**Parameters**:

- `delayMs`: Delay in milliseconds (0 to disable)

**Use case**: Simulating slow networks or testing timeout behavior

### `discardMessagesFrom(bridges, lossProbability)`

**Purpose**: Randomly discards messages with specified probability

**Usage**:

```javascript
node.discardMessagesFrom(otherNode, 0.5); // Drop 50% of messages
node.discardMessagesFrom(otherNode, 1.0); // Drop all messages
node.discardMessagesFrom(otherNode, 0.0); // Drop no messages
```

**Parameters**:

- `lossProbability`: Number between 0.0 (no loss) and 1.0 (total loss)

**Use case**: Simulating unreliable networks or packet loss

## Examples

### Basic Network Partition Test

```javascript
assert.eq(jsTest.options().enableTestCommands, true);

// Set up a replica set with mongobridge
let rst = new ReplSetTest({
  nodes: 3,
  useBridge: true,
  settings: {electionTimeoutMillis: 2000, heartbeatIntervalMillis: 400},
});
rst.startSet();
rst.initiate();

// Partition the primary from secondaries
let primary = rst.getPrimary();
let secondaries = rst.getSecondaries();
primary.rejectConnectionsFrom(secondaries);

// Verify primary steps down due to lost majority
assert.soon(() => {
  return rst.getPrimary() !== primary;
});

// Restore network
primary.acceptConnectionsFrom(secondaries);

rst.stopSet();
```

### Write Concern Timeout Test

```javascript
assert.eq(jsTest.options().enableTestCommands, true);

let st = new ShardingTest({
  shards: {rs0: {nodes: 2}},
  useBridge: true,
});

// Delay replication to cause write concern timeout
st.rs0.getPrimary().delayMessagesFrom(st.rs0.getSecondary(), 10000);

// This write should fail due to timeout
assert.commandFailed(
  st.s0.getCollection("test.coll").insert(
    {x: 1},
    {
      writeConcern: {w: 2, wtimeout: 5000},
    },
  ),
);

// Restore normal replication
st.rs0.getPrimary().delayMessagesFrom(st.rs0.getSecondary(), 0);

st.stop();
```

### Simulating Packet Loss

```javascript
// Set up unreliable network with 30% packet loss
primary.discardMessagesFrom(secondary, 0.3);

// Operations may succeed or fail unpredictably
// Useful for testing retry logic and resilience
```

### Limitations

- **OP_QUERY exhaust**: Not supported for legacy exhaust queries (OP_MSG exhaust cursors are supported)
- **Direct connections**: Only works when connections go through the bridge proxy
- **TLS support**: Mongobridge is not supported if the cluster is using TLS.

## See Also

- [mongobridge.js test example](https://github.com/mongodb/mongo/blob/e810af1916caaedb1cde8d1e1b74bb50b2461daf/jstests/noPassthrough/mongobridge/mongobridge.js#L1)
