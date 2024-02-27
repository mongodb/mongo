# Process Health Checking Library

This module is capable to run server health checks and crash an unhealthy server.

_Note:_ in 4.4 release only the mongos proxy server is supported

## Health Observers

_Health Observers_ are designed for every particular check to run. Each observer can be configured to be on/off and critical or not to be able to crash the serer on error. Each observer has a configurable interval of how often it will run the checks.

## Health Observers Parameters

-   healthMonitoringIntensities: main configuration for each observer. Can be set at startup and changed at runtime. Valid values:

    -   off: this observer if off
    -   critical: if the observer detects a failure, the process will crash
    -   non-critical: if the observer detects a failure, the error will be logged and the process will not crash

    Example as startup parameter:

    ```
    mongos --setParameter "healthMonitoringIntensities={ \"values\" : [{ \"type\" : \"ldap\", \"intensity\" : \"critical\" } ]}"
    ```

    Example as runtime change command:

    ```
    db.adminCommand({ "setParameter": 1,
      healthMonitoringIntensities: {values:
          [{type: "ldap", intensity: "critical"}] } });
    ```

-   healthMonitoringIntervals: how often this health observer will run, in milliseconds.

    Example as startup parameter:

    ```
    mongos --setParameter "healthMonitoringIntervals={ \"values\" : [ { \"type\" : \"ldap\", \"interval\" : 30000 } ] }"
    ```

    here LDAP health observer is configured to run every 30 seconds.

    Example as runtime change command:

    ```
    db.adminCommand({"setParameter": 1, "healthMonitoringIntervals":{"values": [{"type":"ldap", "interval": 30000}]} });
    ```

## LDAP Health Observer

LDAP Health Observer checks all configured LDAP servers that at least one of them is up and running. At every run, it creates new connection to every configured LDAP server and runs a simple query. The LDAP health observer is using the same parameters as described in the **LDAP Authorization** section of the manual.

To enable this observer, use the _healthMonitoringIntensities_ and _healthMonitoringIntervals_ parameters as described above. The recommended value for the LDAP monitoring interval is 30 seconds.

## Active Fault

When a failure is detected, and the observer is configured as _critical_, the server will wait for the configured interval before crashing. The interval from the failure detection and crash is configured with _activeFaultDurationSecs_ parameter:

-   activeFaultDurationSecs: how long to wait from the failure detection to crash, in seconds. This can be configured at startup and changed at runtime.

    Example:

    ```
    db.adminCommand({"setParameter": 1, activeFaultDurationSecs: 300});
    ```

## Progress Monitor

_Progress Monitor_ detects that every health check is not stuck, without returning either success or failure. If a health check starts and does not complete the server will crash. This behavior could be configured with:

-   progressMonitor: configure the progress monitor. Values:

    -   _interval_: how often to run the liveness check, in milliseconds
    -   _deadline_: timeout before crashing the server if a health check is not making progress, in seconds

    Example:

    ```
    mongos --setParameter "progressMonitor={ \"interval\" : 1000, \"deadline\" : 300 }"
    ```
