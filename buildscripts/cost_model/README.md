# The smaller experiment - Calibration with queries that return smaller number of documents. (smaller "n_processed")

This "smaller" experiment differs from the previous experiments that query on collections of a sequence of cardinalities. For calibrating each type of ABT node, we query only against one collection with around a big enough cardinality (e.g. 100k) and a small set of documents that will be queried for collecting calibration data.

There are some differences to conduct this experiment from conducting previous experiments.

## Data generation phase
we need to populate the collections twice - 1) first populate the small set of documents for queries and then 2) populate larger set of documents that will not be queried during most types of ABT nodes calibration. The larger set is for the collections being big enough for queries.

There are more detailed instructions in the "data_generator" settings in "calibration_settings.py".

## Calibration data collection phase
In this experiment, we will run different queries and these queries as mentioned above will mostly be querying the smaller set of documents in order to get a smaller value of "n_processed".

More specifically, to collect data for calibration we only need to run "execute_small_queries()" in "start.py" and make sure to avoid running any other queries outside this function.

## Calibration phase
This phase should be the same as calibration in other experiments.

Please note that this type of experiment may not work with some certain ABT nodes. For example, for PhyscialScan Node, all the data collected may come with the same "n_processed" - the collection cardinality, which does not make any sense for calibration.

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
