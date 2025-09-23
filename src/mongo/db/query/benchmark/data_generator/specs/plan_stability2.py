import dataclasses
import inspect
from collections import OrderedDict
from datetime import datetime, timedelta, timezone
from string import ascii_lowercase
from typing import Callable

import pymongo
from bson.decimal128 import Decimal128
from bson.timestamp import Timestamp
from datagen.util import MISSING, Specification
from faker import Faker

# 75% of the fields will be indexed
NUM_FIELDS = 48

# 50% chance of no correlation
CORRELATIONS = ["a", "b", "c", None, None, None]


class mixed:
    """Used to designate mixed-type fields"""


AVAILABLE_TYPES = [str, int, bool, datetime, Timestamp, Decimal128, list, dict, mixed]

START_DATE = datetime(2024, 1, 1, tzinfo=timezone.utc)
END_DATE = datetime(2025, 12, 31, tzinfo=timezone.utc)

# Ideally we would want to seed our uncorrelated Faker based on the --seed argument to driver.py
# but it is not available here.
ufkr = Faker()
ufkr.seed_instance(1)

universal_generators = {
    "missing": lambda fkr: MISSING,
    "null": lambda fkr: None,
}


def pareto(fkr) -> int:
    """In the absence of a Zipfian implementation to generate skewed datasets, we use pareto"""
    return int(fkr.random.paretovariate(2))


def lambda_sources(l: Specification) -> str:
    """Returns the code of the lambdas that participate in generating the values of a Specification."""
    signature = inspect.signature(l.source)
    params = list(signature.parameters.values())
    return "\n".join(
        f"{probability:>4.0%} {inspect.getsource(generator).strip()}"
        for generator, probability in params[1].default.items()
    )


type_generators: dict[type, dict[str, Callable]] = {
    str: {
        "p1": lambda fkr: ascii_lowercase[min(25, pareto(fkr) % 26)],
        "s1": lambda fkr: fkr.pystr(min_chars=1, max_chars=1),
        "s2": lambda fkr: fkr.pystr(min_chars=1, max_chars=2),
        "s4": lambda fkr: fkr.pystr(min_chars=1, max_chars=4),
    },
    int: {
        "const1": lambda fkr: 1,
        "i10": lambda fkr: fkr.random_int(min=1, max=10),
        "i100": lambda fkr: fkr.random_int(min=1, max=100),
        "i1000": lambda fkr: fkr.random_int(min=1, max=1000),
        "i10000": lambda fkr: fkr.random_int(min=1, max=10000),
        "i100000": lambda fkr: fkr.random_int(min=1, max=100000),
        "pareto": pareto,
    },
    bool: {
        "br": lambda fkr: fkr.boolean(),
        "b10": lambda fkr: fkr.boolean(10),
        "b100": lambda fkr: fkr.boolean(1),
        "b1000": lambda fkr: fkr.boolean(0.1),
        "b10000": lambda fkr: fkr.boolean(0.01),
        "b100000": lambda fkr: fkr.boolean(0.001),
    },
    datetime: {
        "dt_pareto": lambda fkr: START_DATE + timedelta(days=pareto(fkr)),
    },
    Timestamp: {
        # Note that we can not generate timestamps with i > 0 as the i is not preserved in the .schema file
        "ts_const": lambda fkr: Timestamp(fkr.random_element([START_DATE, END_DATE]), 0),
        "ts_triangular": lambda fkr: Timestamp(
            fkr.random.triangular(START_DATE, END_DATE, END_DATE), 0
        ),
    },
    Decimal128: {"decimal_pareto": lambda fkr: Decimal128(f"{pareto(fkr)}.{pareto(fkr)}")},
    list: {
        "list_int_pareto": lambda fkr: [pareto(fkr) for _ in range(pareto(fkr) % 10)],
        "list_str_pareto": lambda fkr: [
            ascii_lowercase[min(25, pareto(fkr) % 26)] for _ in range(pareto(fkr) % 10)
        ],
    },
    dict: {
        "dict_str_pareto": lambda fkr: {
            ascii_lowercase[min(25, pareto(fkr) % 26)]: pareto(fkr) for _ in range(pareto(fkr) % 10)
        }
    },
}

specifications = {}

offset = ufkr.random_int(min=0, max=len(AVAILABLE_TYPES))
for f in range(NUM_FIELDS):
    # Ensure fairness by iterating through the available types,
    # instead of picking a random field type each time. At the same
    # time, the different starting `offset` provides a measure of randomness.
    chosen_type: type = AVAILABLE_TYPES[(offset + f) % len(AVAILABLE_TYPES)]

    if chosen_type is mixed:
        available_generators = [
            generator for type in type_generators.values() for generator in type.values()
        ]
    else:
        available_generators = list(type_generators[chosen_type].values())

    # Add some nulls, missing and the like
    if ufkr.boolean(25):
        available_generators.extend(universal_generators.values())

    chosen_generators: dict[Callable, float] = OrderedDict()
    generator_count = ufkr.random_int(min=2, max=4)

    # Pick the set of generators that will be used for the given field
    for g in range(generator_count):
        generator = ufkr.random_element(available_generators)
        if g < generator_count - 1:
            probability = ufkr.random.uniform(0.0, 0.2)
        else:
            # The final generator receives the remaining weight
            # to arrive to a total weight of 1.
            probability = 1 - sum(chosen_generators.values())
        chosen_generators[generator] = probability

    chosen_correlation = ufkr.random_element(CORRELATIONS)

    # We use the 'default argument value' trick to capture the chosen generators
    # and make them available to the actual value generation.
    def source(fkr, generators=chosen_generators):
        chosen_generator = fkr.random_element(generators)
        return chosen_generator(fkr)

    specification = Specification(chosen_type, correlation=chosen_correlation, source=source)

    # pylint: disable=invalid-name
    field_name = f"field{f}_{chosen_type.__name__}"

    # Have 75% of the fields have an index, which we signify by
    # appending an _idx suffix to the field name.
    if f < NUM_FIELDS * 0.75:
        field_name = field_name + "_idx"

    specifications[field_name] = specification

for field_name, specification in specifications.items():
    print(f"Field {field_name} with type {specification.type.__name__}:")
    print(lambda_sources(specification))
    print()

# Convert the dictionary into a dataclass that driver.py can then use.
plan_stability2 = dataclasses.make_dataclass(
    "plan_stability2",  # Name of the dataclass
    specifications.items(),
)


def indexes() -> list[pymongo.IndexModel]:
    """Return a set of pymongo.IndexModel objects that the data generator will create."""

    indexed_fields = [field_name for field_name in specifications if "idx" in field_name]
    assert len(indexed_fields) > 0

    chosen_indexes: dict[str, pymongo.IndexModel] = {}

    for indexed_field in indexed_fields:
        # The first field of each index is one of the fields we definitely
        # want to be indexed ...
        chosen_fields: dict[str, int] = {
            indexed_field: ufkr.random_element([pymongo.ASCENDING, pymongo.DESCENDING])
        }

        # ... and we will make some indexes multi-field by tacking on more fields.
        secondary_field_count = round(ufkr.random.triangular(low=0, high=2, mode=0))
        for _ in range(secondary_field_count):
            secondary_field = ufkr.random_element(indexed_fields)
            if secondary_field in chosen_fields:
                continue

            has_array_field = any("mixed" in f or "list" in f for f in chosen_fields)

            if ("mixed" in secondary_field or "list" in secondary_field) and has_array_field >= 1:
                # We can not have two array fields in a compound index
                continue

            secondary_dir = ufkr.random_element([pymongo.ASCENDING, pymongo.DESCENDING])

            chosen_fields[secondary_field] = secondary_dir

        chosen_index = pymongo.IndexModel(keys=list(chosen_fields.items()))
        print(chosen_index)

        chosen_indexes[str(chosen_index)] = chosen_index
    return list(chosen_indexes.values())
