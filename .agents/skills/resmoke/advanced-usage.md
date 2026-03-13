# Resmoke Advanced Usage

## Multiversion Testing

Run tests against multiple MongoDB versions:

```bash
# Generate multiversion config
python3 buildscripts/resmoke.py multiversion-config -f multiversion.yml

# Run with multiversion binaries
python3 buildscripts/resmoke.py run --suites=aggregation_multiversion_fuzzer_last_lts \
    --multiversionDir=/path/to/binaries
```

## Tag-based Test Selection

Include/exclude tests by tags:

```bash
# Exclude tests with specific tags
python3 buildscripts/resmoke.py run --suites=core --excludeWithAnyTags=requires_replication,sharding

# Include only tests with specific tags
python3 buildscripts/resmoke.py run --suites=core --includeWithAnyTags=requires_auth
```

Common tags: `requires_auth`, `requires_replication`, `requires_sharding`, `assumes_standalone_mongod`, `assumes_against_mongod_not_mongos`

## Fixture Configuration

Override fixture settings via command line:

```bash
# Replica set with 3 nodes
python3 buildscripts/resmoke.py run --suites=replica_sets --numReplSetNodes=3

# Sharded cluster with 2 shards
python3 buildscripts/resmoke.py run --suites=sharding --numShards=2

# Custom storage engine
python3 buildscripts/resmoke.py run --suites=core --storageEngine=wiredTiger
```

## Server Parameters

Set mongod/mongos parameters:

```bash
python3 buildscripts/resmoke.py run --suites=core \
    --mongodSetParameters='{enableTestCommands: 1, logComponentVerbosity: {command: 2}}'
```

## Repeating Tests

Repeat tests for flake detection:

```bash
# Repeat each test N times
python3 buildscripts/resmoke.py run --suites=core --repeatTests=5

# Repeat for minimum duration
python3 buildscripts/resmoke.py run --suites=core --repeatTestsSecs=300

# Shuffle test order
python3 buildscripts/resmoke.py run --suites=core --shuffle
```

## Running Without Hooks

Disable all test hooks for debugging:

```bash
python3 buildscripts/resmoke.py run --suites=core --noHooks
```

## Powercycle Tests

Test server robustness across power events:

```bash
python3 buildscripts/resmoke.py powercycle --sshConnectionString=user@host
```

## Fuzz Testing

Generate and run with fuzzed configurations:

```bash
# Generate fuzz config
python3 buildscripts/resmoke.py generate-fuzz-config

# Run with fuzzing enabled
python3 buildscripts/resmoke.py run --suites=core --fuzzMongodConfigs=on --configFuzzSeed=12345
```
