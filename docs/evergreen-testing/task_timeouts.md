# Evergreen Task Timeouts

## Types of timeouts

There are two types of timeouts that [Evergreen supports](https://github.com/evergreen-ci/evergreen/wiki/Project-Commands#timeoutupdate):

- **Exec Timeout**: The _exec timeout_ is the overall timeout for a task. Once the total runtime for a test exceeds this value, the timeout logic will be triggered. This value is specified by `exec_timeout_secs` in the Evergreen configuration.
- **Idle Timeout**: The _idle timeout_ is the amount of time Evergreen will wait for output to be generated before considering the task hung and triggering the timeout logic. This value is specified by `timeout_secs` in the Evergreen configuration.

**Note**: In most cases, the **exec timeout** is the more useful of the two timeouts.

## Setting the timeout for a task

There are several ways to set the timeout for a task running in Evergreen.

### Specifying timeouts in the Evergreen YAML configuration

Timeouts can be specified directly in the `evergreen.yml` (and related) files, both for tasks and build variants. This approach is useful for setting default timeout values but is limited because different build variants often have varying runtime characteristics. This means it is not possible to set timeouts for a specific task running on a specific build variant using only this method.

### Overrides: [etc/evergreen_timeouts.yml](../../etc/evergreen_timeouts.yml)

The `etc/evergreen_timeouts.yml` file allows overriding timeouts for specific tasks on specific build variants. This workaround helps address the limitations of directly specifying timeouts in `evergreen.yml`. To use this method, the task must include the `determine task timeout` and `update task timeout expansions` functions at the beginning of its Evergreen definition. Many Resmoke tasks already incorporate these functions.

### Resmoke tasks: [buildscripts/evergreen_task_timeout.py](../../buildscripts/evergreen_task_timeout.py)

This script reads the `etc/evergreen_timeouts.yml` file to calculate the appropriate timeout settings. Additionally, it checks historical test results for the task being run to determine if enough information is available to calculate timeouts based on past data. The script also supports more advanced methods of determining timeouts, such as applying aggressive timeout measures for tasks executed in the commit queue or on required build variants. In cases of conflict, the commit queue and required build variant limits take precedence over the previous two methods.

The timeout that was calculated by the script can be retrieved from the logs:

```
[2025/11/13 15:22:28.292] [2025-11-13 13:22:28,292 - __main__ - INFO] 2025-11-13 13:22.28 Determining timeouts           cli_args=Namespace(install_dir='dist-test/bin', task='aggregation_multiversion_fuzzer_last_lts_0-linux-enterprise', suite_name='aggregation_multiversion_fuzzer_last_lts', variant='enterprise-rhel-8-64-bit-dynamic-required', project='mongodb-mongo-v7.0-staging', evg_alias='evg-alias-absent', timeout=1800, exec_timeout=0, exec_timeout_factor=1.5, outfile='task_timeout_expansions.yml', timeout_overrides_file='etc/evergreen_timeouts.yml', evg_api_config='./.evergreen.yml', evg_project_config='etc/evergreen_nightly.yml')
...
[2025/11/13 15:22:32.622] [2025-11-13 13:22:32,622 - __main__ - INFO] 2025-11-13 13:22.32 Using timeout from cmd line    idle_timeout_secs=1800.0
[2025/11/13 15:22:32.622] [2025-11-13 13:22:32,622 - __main__ - INFO] 2025-11-13 13:22.32 Overriding required-builder timeout exec_timeout_secs=4800.0
[2025/11/13 15:22:32.759] exec_timeout_secs: 4800
[2025/11/13 15:22:32.759] timeout_secs: 1800
```

### Compile tasks: [evergreen/generate_override_timeout.py](../../evergreen/generate_override_timeout.py)

This script is used for compile tasks defined in files such as `etc/evergreen_yml_components/tasks/compile_tasks.yml` and `etc/evergreen_yml_components/tasks/compile_tasks_shared.yml`. The script reads the `etc/evergreen_timeouts.yml` file and calculates appropriate timeouts. The Evergreen function `override task timeout` then runs this script to update the timeouts accordingly.
