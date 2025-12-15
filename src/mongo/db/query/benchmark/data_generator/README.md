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

The following command will add 10 documents to a collection with the namespace `test.Employee`.

```
python3 driver.py specs.employeev2 Employee --db test --size 10
```

The `--indices` argument can be used to create indices from index sets defined in the specification.
Thus,

```
python3 driver.py specs.employeev2 Employee --db test --size 10 --indices index_set_1 --indices index_set_2
```

will generate 10 `Employee` documents _and_ create the indices listed in `index_set_1` and
`index_set_2`.

---

# Specifying a dataset

## Specifying the shape of a document

A document is a JSON object. An object is specified like this (note that the decorator
`@dataclasses.dataclass` is mandatory):

```
@dataclasses.dataclass
class ObjectName:
  field1_name: Specification(dist_func_1)
  field2_name: Specification(dist_func_2)
  field3_name: Specification(dist_func_3)
```

You can nest objects within each other by listing the inner object as the type of a field in the
outer object:

```
@dataclasses.dataclass
class InnerObject:
  <fields>

@dataclasses.dataclass
class OuterObject:
   <fields>
   name_of_nested_object_field: InnerObject
```

## Specifying the distribution of values in a field

For each field in an object, you need to specify a distribution function to populate that field.
There are two ways of doing this:

### 1. Use default distribution for type

If you simply state the type that the field is supposed to be, the data generator will automatically
look for a default distribution for the type:

- For basic python `<type>`s, the default function is the Faker function `py<type>`. See
  https://faker.readthedocs.io/en/master/providers/faker.providers.python.html
- For a nested object type, the data generator will generate instances of the type by looking its definition.

In this example, `InnerObject.f` will be populated by the `faker.pylist` function, and the fully
populated `InnerObject` will be used to populate `OuterObject.f`.

```
@dataclasses.dataclass
class OuterObject:
f: InnerObject

@dataclasses.dataclass
class InnerObject:
f: list[int]
```

### 2. The `Specification` class

The specification class has two positional arguments:

1. `source`, which is the distribution itself. This can take three forms:

   a. A function. The function signature should be `def
func(fac: datagen.util.CorrelatedDataFactory, **kwargs)`. Passing the CorrelatedDataFactory
   through the function allows the values produced to be recorded.

   b. A type. In this case, the default distribution for the type will be used.

   c. A constant. In this case, all fields will be populated using the particular constant. To make
   a field be missing, specify the special variable `MISSING`, which you import from `datagen.util`.

2. `dependson` (optional tuple). The tuple should be populated by strings that match the names of
   fields in the same object as the field that is being specified. The values of the fields that
   match `dependson` will be passed into `source`, along with any values that have been passed down
   from parent objects.

   In the example below, `NestedObject2.i2_1` depends on `NestedObject.i1_1`. Because `dependson` can
   only pass in a value of a field belonging to the same object, the value of `OuterObject.o_1` is
   passed into the distribution function for `OuterObject.o_3` so that the `NestedObject2`
   distribution function can see it.

   ```
    @dataclasses.dataclass
    class NestedObject1:
        i1_1: int


    def func(fac: CorrelatedDataFactory, o_1: NestedObject1, o_2: bool, i2_2: str):
        return f"int {str(o_1.i1_1)} bool {str(o_2)} str {i2_2}"


    @dataclasses.dataclass
    class NestedObject2:
        i2_1: Specification(func, dependson=("i2_2",))
        i2_2: str


    @dataclasses.dataclass
    class OuterObject:
        o_1: Specification(NestedObject1)
        o_2: bool
        o_3: Specification(NestedObject2, dependson=("o_1", "o_2"))
   ```

### External distributions

This data generator supports the following external distributions:

- Any function available to a
  [`random.Random`](https://docs.python.org/3/library/random.html#random.Random) instance. In order
  to use the same seed as the rest of the data generator and take advantage of built-in correlation
  capabilities, you should call the function from `datagen.random.default_random()`.
- Any function available to a [`faker.Faker`](https://faker.readthedocs.io/en/master/index.html)
  instance. In order to use the same seed as the rest of the data generator, you should call the
  function from `datagen.random.global_faker()`.
- Any function available to a [`numpy Random
Generator`](https://numpy.org/doc/stable/reference/random/generator.html) In order
  to use the same seed as the rest of the data generator, you should call the function from
  `datagen.random.numpy_random()`. Note that the numpy Random generator does not support built-in
  correlation capabilities because it uses a rng that we have not figured out how to override.

The external generators `datagen.random.default_random()`, `datagen.random.global_faker()`, etc. are
constructed before your specification file is loaded, and thus you can use these external generators
in your specification file to generate data outside of the context of supplying documents to a
MongoDB collection.

### Creating your own distributions

The fields are populated according to a distribution function. See `datagen.values` for create sets
of values for the distribution function to select over. Basic distribution functions can be found in
`datagen.distribution`. The basic distribution functions can accept constant values, other
functions, or a mix, as their inputs. For example, `uniform(['a', choice(['b', 'c'], [1, 3])])` will
select `'a'` 1/2 of the time, `'b'` 1/8 of the time, or `'c'` 3/8 of the time.

#### Automatic correlation

There is a special distribution function `correlation(distribution, key: str)` that will make the
`distribution` inside correlate 100% with other distributions that have a correlation with the same
key wrapped around it. See ["Example field specifications"](#example-field-specifications).

> Warning: The `correlation` function relies on sharing RNG state between distributions. So the
> function does not work with distribution that are constant values because the RNG is not involved
> in producing constant values.

## Specifying indexes

You can specify sets of indexes to create by adding to the specification file:

1. a function that produces a list of `IndexModel`s.
   ```
   def index_set_1() -> list[pymongo.IndexModel]:
       return [
           pymongo.IndexModel(keys="title.level", name="title_level"),
           pymongo.IndexModel(
               keys=[("title.level", pymongo.DESCENDING), ("salary", pymongo.DESCENDING)],
               name="level_and_salary",
           ),
       ]
   ```
2. a global variable that is a list of
   [pymongo.operations.IndexModel](https://pymongo.readthedocs.io/en/stable/api/pymongo/operations.html#pymongo.operations.IndexModel)s
   ```
   index_set_2 = [
        pymongo.IndexModel(
            keys=[("title.level", pymongo.ASCENDING), ("start_date", pymongo.DESCENDING)],
            name="level_and_start",
        ),
        pymongo.IndexModel(keys="activity.tickets", name="weekly_tickets"),
    ]
   ```

When you generate the dataset, you use the `--indices <variable_or_function_name>` argument to
specify the sets of indexes you want to create with the dataset.

# Running the data generator

## Dropping, dumping, and restoring

Three flags are available to make managing the dataset more easily:

- `--drop` will drop the collection;
- `--restore` will restore the collection from the local dump;
- `--dump` will dump the collection to the local dump.

The `--restore` and `--dump` flags use the `mongorestore` and `mongodump` utilities, respectively,
so it is necessary for them to be installed in order for those flags to function correctly.

To improve reproducibility and comprehension of the generated dataset, every invocation of the data
generator will also copy the specifications file to the dump directory and append the executed
command to a command list file.

### Example

After setting up a `mongod` at `localhost:27017`, one can run:

```
python3 driver.py specs.employee Employee --size 10000 --drop --dump
```

to drop, generate, and dump the `Employee` dataset.

One might also want to restore the original dump first with:

```
python3 driver.py specs.employee Employee --size 10000 --drop --dump --restore
```

By default, `--dump` and `--restore` make user of the `out/` subdirectory as the output directory.
`--out`/`-o` can be passed in to change the directory as needed.

### Order of operations

If any flags are set, the order in which they operate is:

1. The collection is dropped (`--drop`);
2. The collection is restored (`--restore`);
3. New data are generated into the collection (controlled by `-n`);
4. Indexes are created (controlled by `--indexes`);
5. Specifications and commands are snapshotted (if any of the above steps changed any data); and
6. The collection is dumped (`--dump`).

Only the flags that are set will be executed. Since the `-n` argument defaults to `0`, omitting a
value for `-n` will cause no data to be generated.

### Reproducibility

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
- Adds an index set with `--indexes`, or
- Generates at least one document with positive `--size`.

In theory, this means that someone else can analyze the dumped `commands.sh` file and the copied
specifications files to understand how the dataset was generated.

Unless otherwise specified with `--out`/`-o`, the default dump directory is `./out`.

**To avoid infinite loop errors when directly executing `bash out/commands.sh`, the `--dump` flag
itself is never logged.**

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

# Example field specifications

## Generating completely uncorrelated data

Completely uncorrelated data is generated if a given specification does not use the `correlation` distribution:

```python
@dataclasses.dataclass
class Uncorrelated:
    field1: Specification(source=uniform(["a", "b"]))
    field2: Specification(source=uniform(["a", "b"]))
```

The configuration above produces a collection where `field1` and `field2` are completely uncorrelated
so that all combinations of values are equally likely:

```
Enterprise test> db.Uncorrelated.aggregate([{ $group: { _id: {field1: "$field1", field2:"$field2"}, count: { $count: {} }}}, {$sort: {"_id.field1":1, "_id.field2":1}}] ) ;
[
  { _id: { field1: 'a', field2: 'a' }, count: 223 },
  { _id: { field1: 'a', field2: 'b' }, count: 271 },
  { _id: { field1: 'b', field2: 'a' }, count: 271 },
  { _id: { field1: 'b', field2: 'b' }, count: 235 }
]
```

## Generating partially correlated data

By default, if a field specification is configured to use a particular correlation, all data generated
for it will be 100% correlated. To mix in some uncorrelated data, create a `choice` between a
`correlation` distribution and a non-`correlation` distribution:

```python
from datagen.distribution import correlation, uniform

@dataclasses.dataclass
class PartiallyCorrelated:
    field1: Specification(correlation(uniform(["a", "b"]), "correlation1"))
    field2: Specification(
        choice([correlation(uniform(["a", "b"]), "correlation1"), uniform(["c", "d"])], [9, 1])
    )
```

For `field2`, 90% of the values will be either `a` or `b`, correlated to the value of `field1` and
10% of the values will be either `c` or `d`, not correlated to anything:

```
Enterprise test> db.Uncorrelated.aggregate([{ $group: { _id: {field1: "$field1", field2:"$field2"}, count: { $count: {} }}}, {$sort: {"_id.field1":1, "_id.field2":1}}] ) ;
[
  { _id: { field1: 'a', field2: 'a' }, count: 423 },
  { _id: { field1: 'a', field2: 'c' }, count: 25 },
  { _id: { field1: 'a', field2: 'd' }, count: 25 },
  { _id: { field1: 'b', field2: 'b' }, count: 469 },
  { _id: { field1: 'b', field2: 'c' }, count: 33 },
  { _id: { field1: 'b', field2: 'd' }, count: 25 }
]
```

## Generating objects

There are two ways to generate objects -- either directly from a Python dict or from a class that is defined in the spec.

### Generating from a python dict

```python
from datagen.random import faker

@dataclasses.dataclass
class DictSpec:
    dict_field: Specification(dict, source=lambda: {'a': global_faker().random_int(1, 10)})
```

In the output `.schema` file, this is going to look as follows:

```
"dict_field": {
    ...
    "types": {
        "obj": {
            "min": {
                "a": 1
            },
            "max": {
                "a": 10
            },
            "unique": [
                {
                    "a": 1
                }
            ...
            ]
        }
    }
}
```

### Generating using a child class

```python

@dataclasses.dataclass
class ChildObject:
    str_field: Specification(str, source=lambda: "A")

@dataclasses.dataclass
class ParentObject:
    child_field: Specification(ChildObject)
```

produces the following `.schema` fragment:

```
"obj_field": {
    ...
    "nested_object": {
        "str_field": {
            ...
            "types": {
                "str": {
                    "min": "A",
                    "max": "A",
                    "unique": [
                        "A"
                    ]
                }
            }
        }
    }
},
```
