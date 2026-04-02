# Test Composer

Test Composer uses an opinionated naming convention to let Antithesis autonomously generate
thousands of test cases across multiple system states — controlling parallelism, fault injection,
and command ordering without you having to specify each scenario manually. See the
[Antithesis docs](https://antithesis.com/docs/test_templates/) for the upstream documentation.

Each subdirectory of this directory is a self-contained template: a set of executable shell scripts
whose filename prefix tells Antithesis how and when to run them. The setup still uses a resmoke
suite to determine cluster topology, but test execution is driven by these scripts rather than
jstests directly.

For background on Antithesis, the base images, and the broader CI pipeline, see
[docs/antithesis/README.md](../../../docs/antithesis/README.md).

## Script naming conventions

Scripts must be executable and live directly under the template directory (not in subdirectories).
The prefix of the filename determines scheduling behavior. Any file that doesn't match a known
prefix — including files in subdirectories or files prefixed with `helper_` — is ignored by
Test Composer and can be used for shared logic.

### Driver commands

Run during fault injection periods. At least one driver or `anytime_*` command is required.

- **`parallel_driver_<name>`** — runs concurrently with other parallel drivers, including itself.
  Use for continuous client operations, parallel workloads, and availability checks under faults.

- **`singleton_driver_<name>`** — runs as the only active driver in a history branch.
  Use for porting existing integration tests or workloads that shouldn't overlap with other drivers.

- **`serial_driver_<name>`** — runs only when no other driver commands are active.
  Use for validation steps and operations that require quiescence.

### Quiescent commands

Run in the absence of faults.

- **`first_<name>`** — optional one-time setup that runs once before any driver commands start.
  Use for data initialization, schema setup, and bootstrapping.

- **`eventually_<name>`** — runs after driver commands start; halts all drivers and stops faults,
  creating a new history branch. Use for testing eventual consistency and post-recovery state.
  Always include retry loops, since the cluster may need time to become available.

- **`finally_<name>`** — like `eventually_*`, but only runs after all driver commands complete
  naturally. Use for final consistency checks and subtle invariant validation.

### Advanced commands

- **`anytime_<name>`** — can run at any point after startup, even alongside singleton/serial
  drivers. Use for continuous invariant checks and corruption detection.

## Templates

### `basic_js_commands`

Parallel JavaScript workload against a single `mongod`. All commands share retry logic defined in
[`js/commands.js`](basic_js_commands/js/commands.js) that handles transient network errors,
server selection failures, and retryable write errors.

| Script                                           | Function                      | Notes                                                                       |
| ------------------------------------------------ | ----------------------------- | --------------------------------------------------------------------------- |
| `parallel_driver_mongod_find.sh`                 | Random `findOne` queries      |                                                                             |
| `parallel_driver_mongod_insert.sh`               | Random `insertOne`            |                                                                             |
| `parallel_driver_mongod_fsync.sh`                | `fsync` admin command         |                                                                             |
| `parallel_driver_mongod_pitread.sh`              | Point-in-time snapshot reads  | Validates older snapshot value is returned                                  |
| `parallel_driver_mongod_validate_collections.sh` | `validate` on all collections |                                                                             |
| `parallel_driver_mongod_aggregate.sh`            | Aggregation pipelines         | Exercises `$match`, `$group`, `$sort`, `$project`, `$lookup`, `$unwind`     |
| `parallel_driver_mongod_update.sh`               | `updateOne`/`updateMany`      | Uses `$set`, `$inc`, `$push` with random data                               |
| `parallel_driver_mongod_delete.sh`               | `deleteOne`/`deleteMany`      | Tight filters to avoid wiping all documents                                 |
| `parallel_driver_mongod_txn.sh`                  | Multi-document transactions   | Retries on `TransientTransactionError` and `UnknownTransactionCommitResult` |
| `anytime_mongod_dbcheck.sh`                      | `dbCheck` on all collections  | Detects data corruption; safe to run at any time                            |

### `random_resmoke`

Runs existing resmoke jstests with random seeds and shuffling, adapting the standard test
infrastructure for Test Composer. Both scripts use
`--seed $(od -vAn -N4 -tu4 < /dev/urandom) --shuffle --sanityCheck`.

| Script                        | Function                                                         |
| ----------------------------- | ---------------------------------------------------------------- |
| `singleton_driver_resmoke.sh` | Runs a single random resmoke test alone                          |
| `serial_driver_resmoke.sh`    | Runs resmoke tests sequentially when no other drivers are active |

## Best practices

- **Retry logic** — always handle transient network errors and server selection failures.
  See [`commands.js`](basic_js_commands/js/commands.js) for a reusable retry wrapper.
- **Randomize** — the more variation you introduce, the more state space Antithesis can explore.
  Antithesis controls and can reproduce the random seed, so interesting paths can be re-explored.
- **Idempotency** — design scripts to tolerate being killed and restarted at any point.
- **Start simple** — begin with a `singleton_driver_*` to port an existing test, then evolve
  toward parallel drivers as confidence grows.

## Running locally

Build the Docker images for a suite using resmoke. This generates a `docker_compose/<suite>/`
directory containing a `docker-compose.yml` and all scripts.

```bash
python3 buildscripts/resmoke.py run \
  --suite <suite_name> \
  --dockerComposeBuildImages workload,config,mongo-binaries \
  --dockerComposeTestComposerDirs basic_js_commands
```

Pass multiple templates as a comma-separated list:

```bash
--dockerComposeTestComposerDirs basic_js_commands,random_resmoke
```

Start the topology:

```bash
docker compose -f docker_compose/<suite_name>/docker-compose.yml up
```

Run a single command inside the workload container:

```bash
docker compose -f docker_compose/<suite_name>/docker-compose.yml \
  run --rm workload \
  /opt/antithesis/test/v1/basic_js_commands/parallel_driver_mongod_aggregate.sh
```

The `/scripts/print_connection_string.sh` helper used by each script is generated automatically
from the resmoke fixture's connection string and placed in the config image during the build step.

## Adding a new template

1. Create `buildscripts/antithesis/test_composer/<your_template>/`

2. Write executable scripts with the appropriate prefix:

   ```bash
   #!/usr/bin/env bash
   # parallel_driver_mytest.sh — runs concurrently with other parallel drivers

   CONNECTION_URL=$(bash /scripts/print_connection_string.sh)
   # ... your test logic here
   ```

3. `chmod +x` all scripts.

4. Put any shared logic in a subdirectory or use a `helper_` prefix — Test Composer ignores both.

5. Reference the directory name in your Evergreen task:

   ```yaml
   - func: "antithesis image build and push"
     vars:
       suite: <resmoke_suite> # still controls cluster topology
       antithesis_test_composer_dir: <your_template>
   ```

   Full Evergreen task example:

   ```yaml
   - <<: *antithesis_task_template
     name: antithesis_my_task
     commands:
       - func: "antithesis image build and push"
         vars:
           suite: concurrency_sharded_replication_with_balancer_and_config_transitions_and_add_remove_shard
           resmoke_args: >-
             --runAllFeatureFlagTests
           antithesis_test_composer_dir: basic_js_commands
   ```
