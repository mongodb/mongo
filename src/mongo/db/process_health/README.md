# Process Health Checking Library

This module is capable to run server health checks and crash an unhealthy server.

*Note:* in 4.4 release only the mongos proxy server is supported

## Health Observers

*Health Observers* are designed for every particular check to run. Each observer can be configured to be on/off and critical or not to be able to crash the serer on error. Each observer has a configurable interval of how often it will run the checks.




