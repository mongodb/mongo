-   All end-to-end resmoke tests can be run via a resmoke suite itself:

```
mongodb_repo_root$ /opt/mongodbtoolchain/v4/bin/python3 buildscripts/resmoke.py run --suites resmoke_end2end_tests
```

-   Finer grained control of tests can also be run with by invoking python's unittest main by hand. E.g:

```
mongodb_repo_root$ /opt/mongodbtoolchain/v4/bin/python3 -m unittest -v buildscripts.tests.resmoke_end2end.test_resmoke.TestTestSelection.test_at_sign_as_replay_file
```
