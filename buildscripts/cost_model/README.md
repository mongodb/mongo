# Cost Model Calibrator

## Python virtual environment

The following assumes you are using python from the MongoDB toolchain.

```
/opt/mongodbtoolchain/v4/bin/python3
```

### Getting started

```sh
(mongo-python3) deactivate  # only if you have another python env activated
sh> /opt/mongodbtoolchain/v4/bin/python3 -m venv cm  # create new env
sh> source cm/bin/activate  # activate new env
(cm) python -m pip install -r requirements.txt  # install required packages
(cm) python start.py  # run the calibrator
(cm) deactivate  # back to bash
sh>
```

### Install new packages

```sh
(cm) python -m pip install <package_name>     # install <package_name>
(cm) python -m pip freeze > requirements.txt  # do not forget to update requirements.txt
```
