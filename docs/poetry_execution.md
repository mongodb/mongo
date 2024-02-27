# Poetry Project Execution

## Project Impetus

We frequently encounter Python errors that are caused by a python dependency author updating their package that is backward breaking. The following tickets are a few examples of this happening:
[SERVER-79126](https://jira.mongodb.org/browse/SERVER-79126), [SERVER-79798](https://jira.mongodb.org/browse/SERVER-79798), [SERVER-53348](https://jira.mongodb.org/browse/SERVER-53348), [SERVER-57036](https://jira.mongodb.org/browse/SERVER-57036), [SERVER-44579](https://jira.mongodb.org/browse/SERVER-44579), [SERVER-70845](https://jira.mongodb.org/browse/SERVER-70845), [SERVER-63974](https://jira.mongodb.org/browse/SERVER-63974), [SERVER-61791](https://jira.mongodb.org/browse/SERVER-61791), and [SERVER-60950](https://jira.mongodb.org/browse/SERVER-60950). We have always known this was a problem and have known there was a way to fix it. We finally had the bandwidth to tackle this problem.

## Project Prework

First, we wanted to test out using poetry so we converted mongo-container project to use poetry [SERVER-76974](https://jira.mongodb.org/browse/SERVER-76974). This showed promise and we considered this a green light to move forward on converting the server python to use poetry.

Before we could start the project we had to upgrade python to a version that was not EoL. This work is captured in [SERVER-72262](https://jira.mongodb.org/browse/SERVER-72262). We upgraded python to 3.10 on every system except windows. Windows could not be upgraded due to a test problem relating to some cipher suites [SERVER-79172](https://jira.mongodb.org/browse/SERVER-79172).

## Conversion to Poetry

After the prework was done we wrote, tested, and merged [SERVER-76751](https://jira.mongodb.org/browse/SERVER-76751) which is converting the mongo python dependencies to poetry. This ticket had an absurd amount of dependencies and required a significant amount of patch builds. The total number of changes was pretty small but it affected a lot of different projects.

Knowing there was a lot this touched we expected to see some bugs and were quick to try to fix them. Some of these were caught before merge and some were caught after.

[BUILD-17860](https://jira.mongodb.org/browse/BUILD-17860) required the build team to rebuild python on macosx arm. This was caught before merging.

[SERVER-81122](https://jira.mongodb.org/browse/SERVER-81122) found that poetry broke the spawnhost script. This was caught after merge.

[SERVER-81061](https://jira.mongodb.org/browse/SERVER-81061) and [BF-29909](https://jira.mongodb.org/browse/BF-29909) were found by sys-perf since they run their own build and do not use the standard build process. Therefor it was very hard to test for this one. This was caught post merge.

[SERVER-80799](https://jira.mongodb.org/browse/SERVER-80799) found that poetry broke mongo tooling metrics collection (not OTel). This was only found since an engineer on the team saw this bug in the code. This was caught post merge.

Overall, when changing something so foundational it is inevitable that some things will break.
