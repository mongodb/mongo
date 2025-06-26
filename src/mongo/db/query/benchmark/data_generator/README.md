# Query Data Generator

This is a data generation framework for Query benchmarks.
It is intended to be a way to easily produce workloads for Query testing use cases.
Key features of this work:

- Data schema are declarative. Relationships are interspersed in the schema, however.
- Correlations are easy to model. This is done through a combination of three mechanisms.
- `faker` is a data source. This makes it easier to generate more realistic and comprehensible data.

---

# Quickstart

Create a virtual environment and activate it.

```
python3 -m venv venv --prompt 'datagen'
source venv/bin/activate
```

Now install the dependencies.

```
pip install -r requirements.txt
```

Create some `Employee`s!

```
python3 driver.py specs.employee Employee -n 10
```

---

# Basic working example

`specs/employee.py` contains an example of a specification that can be used to generate some data.

Invoking

```
python3 driver.py specs.employee Employee
```

will generate a single `Employee` object.

The `-n` flag can be used to generate multiple objects, e.g.

```
python3 driver.py specs.employee Employee -n 10
```

will generate 10 `Employee` objects.

The `--indices` switch can be used to create indices from index sets defined in the specification.
Thus,

```
python3 driver.py specs.employee Employee -n 10 --indices index_set_1 --indices index_set_2
```

will generate 10 `Employee` objects _and_ create the indices listed in `index_set_1` and
`index_set_2`.

# Dropping, dumping, and restoring

Three flags are available to make managing the dataset more easily:

- `--drop` will drop the collection;
- `--restore` will restore the collection from the local dump;
- `--dump` will dump the collection to the local dump.

The `--restore` and `--dump` flags use the `mongorestore` and `mongodump` utilities, respectively,
so it is necessary for them to be installed in order for those flags to function correctly.

To improve reproducibility and comprehension of the generated dataset, every invocation of the data
generator will also copy the specifications file to the dump directory and append the executed
command to a command list file.

## Example

After setting up a `mongod` at `localhost:27017`, one can run:

```
python3 driver.py specs.employee Employee -n 10000 --drop --dump
```

to drop, generate, and dump the `Employee` dataset.

One might also want to restore the original dump first with:

```
python3 driver.py specs.employee Employee -n 10000 --drop --dump --restore
```

By default, `--dump` and `--restore` make user of the `out/` subdirectory as the output directory.
`--out`/`-o` can be passed in to change the directory as needed.

## Order of operations

If any flags are set, the order in which they operate is:

1. The collection is dropped (`--drop`);
2. The collection is restored (`--restore`);
3. New data are generated into the collection (controlled by `-n`);
4. Indices are created (controlled by `--indices`);
5. Specifications and commands are snapshotted (if any of the above steps changed any data); and
6. The collection is dumped (`--dump`).

Only the flags that are set will be executed. Since the `-n` argument defaults to `0`, omitting a
value for `-n` will cause no data to be generated.

## Reproducibility

If an invocation generates at least one object, the specifications file is copied over to the dump
directory.

Additionally, each command that changes the dataset in some way is logged in a local `commands.sh`
file.

- If no `--seed` was passed in, the data generator selects its own seed and appends it to the logged
  command.
- If the `--drop` flag is set, the local `commands.sh` file is cleared.
- If the `--dump` flag is set, the local `commands.sh` file is appended to the `commands.sh` file in
  the dump directory; the local `commands.sh` file is subsequently deleted.

Commands that are considered to change the dataset are ones that:

- Contains either `--drop` or `--restore`, or
- Adds an index set with `--indices`, or
- Generates at least one document with positive `--num`.

In theory, this means that someone else can analyze the dumped `commands.sh` file and the copied
specifications files to understand how the dataset was generated.

Unless otherwise specified with `--out`/`-o`, the default dump directory is `./out`.

**To avoid infinite loop errors when directly executing `bash out/commands.sh`, the `--dump` flag
itself is never logged.**

## Syncing with AWS S3

### Using the helper script

There is a
[helper script](https://github.com/10gen/employees/blob/master/home/william.qian/SPM-2658/s3.sh)
that can be used to simplify the interactions with S3.
Download the script and run it without any arguments to print the help text.
For convenience, it's helpful to put the script somewhere along your `PATH` (or add the parent
directory to your `PATH`).

At the moment, example functionalities are:

- SSO setup: `s3.sh sso` (see the next subsubsection for first-time SSO setup)
- List data corpora: `s3.sh ls`
- Recursively list a corpus with human-readable numbers: `s3.sh ls -Rh genA-10K`
- Pull a corpus: `s3.sh pull genA-10K`
- Push a corpus: `s3.sh push genA-10K`

### Setting up AWS CLI credentials

Run

```
aws configure sso
```

using starting URL

```
https://d-9067613a84.awsapps.com/start/#
```

and region

```
us-east-1
```

It is recommended to name the session and profile something you can remember easily (e.g. `qbd` for
`Query Benchmark Data` or something similarly memorable).
Once you have followed the workflow to completion, using `us-east-1` as the profile region and
`None` as the output format, continue to the next session.

**You will need to rerun `aws configure sso` whenever your credentials expire.**
Using a named session, you do not have to re-enter the starting URL in subsequent auth loops.

### Pulling a dataset from S3

> If you named your AWS profile something other than `qbd`, replace `qbd` below with the profile
> name you used or were given (if you did not actively choose one).
> You can find a list of profiles in `~/.aws/config`.

Running this will pull the `corpora/repo_test` dataset into your local `dump` directory:

```
aws --profile=qbd s3 sync s3://query-benchmark-data/corpora/repo_test dump
```

You should be able to run

```
python3 driver.py specs.employee Employee --drop --restore
```

to load the dataset.

### Pushing a dataset to S3

> If you named your AWS profile something other than `qbd`, replace `qbd` below with the profile
> name you used or were given (if you did not actively choose one).
> You can find a list of profiles in `~/.aws/config`.

After running a dump command like

```
python3 driver.py specs.employee Employee --dump
```

to dump the dataset and associated commands, you can run

```
aws --profile=qbd s3 sync dump s3://query-benchmark-data/corpora/repo_test
```

to push the dataset in your `dump` directory to the S3 prefix `corpora/repo_test`.

---

# How it works

This suite tackles the problem of generating correlated data using two strategies: linear scaling
and derivation.

## Theory

### Linear scaling

In linear scaling, two fields `a` and `b` (drawn from discrete, ordered sets) are correlated
indirectly by instead correlating them to a hidden value `n`.
Each distinct `n` maps to a potentially non-unique (`a`, `b`) pair.
In the common case, as `n` increases, so do the values of `a` and `b`, though they might not
increase at the same rates as each other.
Any linear correlation can be represented this way, including inverse correlations (by simply
reversing the order of traversal for that field).

This diagram provides a simple visual of how this works:

```
a: a0       a1    a2                 a3 a4            a5
n: ---------------------------------------------------------
b: b0    b1         b2          b3 b4   b5        b6     b7
```

Notably, given `x = (ax, bx)` and `y = (ay, by)`, it's clear that if `ax < ay`, then `bx <= by`.
Thus, `a` and `b` are directly correlated.

### Derivation

Linear scaling alone is insufficient for representing many of the correlations in which we are
interested.
For example, salary might depend on not only the employee's level, but also their team and track
(whether they are a manager, etc.).
To represent such a correlation, one might think about salary as a multivariate function of the
employee's level, team, and track, such as:

```
f: (level, team, track) -> salary
```

Fortunately, this is fairly easy to model in Python as... regular functions.

## Implementation

Linear scaling is primarily implemented using a custom random number generator,
`datagen.random.CorrelatedRng`.
This subclass of `random.Random` makes two important changes:

1. Replaces modulus-based arithmetic with multiplication-based arithmetic.
2. Maintains a cache of states so that random number generations are deterministically repeatable.
