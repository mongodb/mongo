First, activate the virtual environment:

```
mongodb_repo_root$ source python3-venv/bin/activate
```

- All end-to-end resmoke tests can be run via a resmoke suite itself:

```
(python3-venv) mongodb_repo_root$ python buildscripts/resmoke.py run --suites resmoke_end2end_tests
```

- Finer grained control of tests can also be run with by invoking python's unittest main by hand. E.g:

```
(python3-venv) mongodb_repo_root$ python -m unittest -v buildscripts.tests.resmoke_end2end.test_resmoke.TestTestSelection.test_at_sign_as_replay_file
```
