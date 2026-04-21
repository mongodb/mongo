# Cost Model Calibrator

## Getting Started

### 1) Setup Mongod

First, prepare the MongoDB server:

1. Activate the standard virtual environment:

```sh
source python3-venv/bin/activate
```

2. Build server with optimizations (makes doc insertion faster):

```sh
(python3-venv) bazel build --config=opt install-devcore
```

3. Run mongod instance (only for CBR calibration, because join_start.py manages mongod's lifecycle
   itself):

```sh
(python3-venv) bazel-bin/install-mongod/bin/mongod --setParameter internalMeasureQueryExecutionTimeInNanoseconds=true
```

### 2) Setup Cost Model Calibrator

In another terminal:

1. Navigate to the cost model directory:

```sh
cd buildscripts/cost_model
```

2. Set up Python alias to use MongoDB toolchain:

```sh
alias python=/opt/mongodbtoolchain/v4/bin/python3
```

3. Deactivate any existing Python environment (if needed):

```sh
deactivate
```

4. Create new virtual environment:

```sh
/opt/mongodbtoolchain/v4/bin/python3 -m venv cm
```

5. Activate the new environment:

```sh
source cm/bin/activate
```

6. Install required packages:

```sh
(cm) python -m pip install -r requirements.txt
```

7. Run the calibrator:

- For CBR cost model calibration:
  ```sh
  (cm) python start.py
  ```
- For JOO cost model calibration:
  ```sh
  (cm) python join_start.py
  ```
  To skip the constant calibration (warm scan, CPU, sequential I/O, random I/O) and only run the
  join algorithm comparison:
  ```sh
  (cm) python join_start.py --join-only
  ```
  To iterate quickly on cost model changes, reuse pre-recorded execution times from a previous full
  run. This skips actual query execution, only running `queryPlanner` explains to collect fresh cost
  estimates:
  ```sh
  (cm) python join_start.py --execution-times join_output/join_times_in-cache.csv join_output/join_times_exceeds-cache.csv
  ```

**Note:** For CBR calibration, the first time it will take a while since it has to generate the
data. Afterwards, as long as you aren't modifying the collections, you can comment out
`await generator.populate_collections()` in `start.py` - this will make it a lot faster.

8. When done, deactivate the environment:

```sh
(cm) deactivate
```

## Install New Packages

1. Install the package:

```sh
(cm) python -m pip install <package_name>
```

2. Update requirements.txt:

```sh
(cm) python -m pip freeze > requirements.txt
```
