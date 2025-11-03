# Connection

## Background Compaction

### Overview

The background compaction thread is implemented in [conn_compact.c](conn_compact.c).

### Compaction Heuristics

They key heuristics used to determine whether a file should be compacted are covered in [__background_compact_should_skip](https://github.com/wiredtiger/wiredtiger/blob/cec2d68c2a51be610745b4ab481c572aaec08d3c/src/conn/conn_compact.c#L233-L311).

These include (order matters):
1. A table can be configured to be skipped by compaction using the parameter `exclude=["table:a.wt"]`, the configuration is parsed in [__background_compact_exclude_list_process](https://github.com/wiredtiger/wiredtiger/blob/cec2d68c2a51be610745b4ab481c572aaec08d3c/src/conn/conn_compact.c#L100-L120) , you can find an example in [test_compact06](https://github.com/wiredtiger/wiredtiger/blob/cec2d68c2a51be610745b4ab481c572aaec08d3c/test/suite/test_compact06.py#L38)
1. File size < `1MB`
1. `background_compact.max_file_skip_time` (unit of Seconds). If the elapsed time since the last compaction run for the same URI has exceeded this threshold, the file is selected for compaction.
   1. The configuration of `debug_mode.background_compact` controls the value, we have [hardcoded configurations](https://github.com/wiredtiger/wiredtiger/blob/cec2d68c2a51be610745b4ab481c572aaec08d3c/src/conn/conn_api.c#L2158-L2176).
1. A previous compaction failure occurred. 
   1. Assume the last compaction failed — retrying too soon is likely to fail again, so the retry is skipped to prevent repeated failures.
   1. Combined with the previous condition, this means that within each `max_file_skip_time` period, there can be at most one failed compaction.

### Removing a compaction stat from the connection list

The cleanup logic is implemented in [__background_compact_list_cleanup](https://github.com/wiredtiger/wiredtiger/blob/cec2d68c2a51be610745b4ab481c572aaec08d3c/src/conn/conn_compact.c#L412-L447).

There are three types of cleanup as indicated by `cleanup_type`:
- For `EXIT` and `OFF` type, the function will clear all the stats directly.
- For `STALE_STAT` type, the function attempts to remove statistics that have been idle for a long time. 
   - The entrance is from the [__background_compact_server](https://github.com/wiredtiger/wiredtiger/blob/cec2d68c2a51be610745b4ab481c572aaec08d3c/src/conn/conn_compact.c#L572-L574)
   - Idle time metrics include:
      - `background_compact.max_file_idle_time` (seconds), used to remove tracking of files that have not been updated after a long time (usually a result of file deletion).
      - `max_file_skip_time`, max compact wait time from last compact run. The debug mode `debug_mode.background_compact` provides a shorter time period for testing purposes, see the [hardcoded configurations](https://github.com/wiredtiger/wiredtiger/blob/cec2d68c2a51be610745b4ab481c572aaec08d3c/src/conn/conn_api.c#L2158-L2176). 

