# Evergreen Task Timeouts

## Type of timeouts

There are two types of timeouts that [evergreen supports](https://github.com/evergreen-ci/evergreen/wiki/Project-Commands#timeoutupdate):

-   **Exec timeout**: The _exec_ timeout is the overall timeout for a task. Once the total runtime for
    a test hits this value, the timeout logic will be triggered. This value is specified by
    **exec_timeout_secs** in the evergreen configuration.
-   **Idle timeout**: The _idle_ timeout is the amount of time in which evergreen will wait for
    output to be created before it considers the task hung and triggers timeout logic. This value
    is specified by **timeout_secs** in the evergreen configuration.

**Note**: In most cases, **exec_timeout** is usually the more useful of the timeouts.

## Setting the timeout for a task

There are a few ways in which the timeout can be determined for a task running in evergreen.

-   **Specified in 'etc/evergreen.yml'**: Timeout can be specified directly in the 'evergreen.yml' file,
    both on tasks and build variants. This can be useful for setting default timeout values, but is limited
    since different build variants frequently have different runtime characteristics and it is not possible
    to set timeouts for a task running on a specific build variant.

-   **etc/evergreen_timeouts.yml**: The 'etc/evergreen_timeouts.yml' file for overriding timeouts
    for specific tasks on specific build variants. This provides a work-around for the limitations of
    specifying the timeouts directly in the 'evergreen.yml'. In order to use this method, the task
    must run the "determine task timeout" and "update task timeout expansions" functions at the beginning
    of the task evergreen definition. Most resmoke tasks already do this.

-   **buildscripts/evergreen_task_timeout.py**: This is the script that reads the 'etc/evergreen_timeouts.yml'
    file and calculates the timeout to use. Additionally, it will check the historic test results of the
    task being run and see if there is enough information to calculate timeouts based on that. It can
    also be used for more advanced ways of determining timeouts (e.g. the script is used to set much
    more aggressive timeouts on tasks that are run in the commit-queue).
