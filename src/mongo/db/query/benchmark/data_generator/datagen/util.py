# Copyright (C) 2025-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.

"""Utilities for data generation."""

import collections
import contextlib
import dataclasses
import enum
import typing

import datagen.faker
import datagen.random
import datagen.statistics
import faker
from datagen.database_instance import DatabaseInstance

####################################################################################################
#
# A global Faker object used to generate uncorrelated data
#
####################################################################################################

UNCORRELATED_FAKER = None


def set_uncorrelated_faker(faker):
    global UNCORRELATED_FAKER
    if UNCORRELATED_FAKER is None:
        UNCORRELATED_FAKER = faker


def uncorrelated_faker():
    assert UNCORRELATED_FAKER is not None
    return UNCORRELATED_FAKER


####################################################################################################
#
# Correlated data generator definitions begin here.
#
####################################################################################################


@dataclasses.dataclass
class CorrelatedContext(contextlib.ContextDecorator):
    """This is a context that makes it easier to implement correlated data. For example,
    with CorrelatedContext(self.generator, 'a context key'):
        ...

    is equivalent to

    self.generator.recall('a context key')
    ...
    self.generator.reset()

    but with a lower likelihood of forgetting the `.reset()`.
    """

    resource: datagen.faker.CorrelatedGenerator | datagen.random.CorrelatedRng
    name: str | None

    def __enter__(self):
        if self.name:
            self.resource.recall(self.name)

    def __exit__(self, *_):
        if self.name:
            self.resource.reset()


class CorrelatedDataFactory:
    """This is a factory class for producing randomly-generated correlated data."""

    def __init__(self, provider: faker.providers.BaseProvider, fkr: faker.proxy.Faker):
        self.faker = fkr
        self.provider = provider
        self.random = provider.generator.random
        self.statistics = datagen.statistics.StatisticsRegister()

    def build(self, obj_type: type):
        """Produce a randomly-generated value of the desired type."""
        hints = typing.get_type_hints(obj_type)
        if hints:
            fields = {}
            queue = []

            def queue_items():
                for k, v in hints.items():
                    yield k, v
                for k, v in queue:
                    yield k, v

            for field, hint in queue_items():
                with self.statistics.path_cm(field):
                    dependencies = {}
                    if isinstance(hint, Specification):
                        if any(dependency not in fields for dependency in hint.dependson):
                            queue.append((field, hint))
                            continue
                        dependencies = {
                            dependency: fields[dependency] for dependency in hint.dependson
                        }
                    field_value = self.build_using_hint(obj_type, field, hint, dependencies)
                    fields[field] = field_value
            self.statistics.register_fields(fields)
            return obj_type(**fields)
        # From here, generate some basic types by default.
        elif issubclass(obj_type, enum.Enum):
            return self.provider.enum(obj_type)
        elif hasattr(obj_type, "__args__"):
            return getattr(self.provider, f"py{obj_type.__name__}")(value_types=obj_type.__args__)
        else:
            return getattr(self.provider, f"py{obj_type.__name__}")()

    def build_using_hint(self, obj_type: type, field: str, hint, dependencies):
        """Wrap around build() to make use of type hints in the specification."""
        if isinstance(hint, Specification):
            with CorrelatedContext(self.random, hint.correlation):
                if hint.source and hasattr(obj_type, f"make_{field}"):
                    raise RuntimeError(
                        f"Field {field} has both a hint source and a make function, but should only have one."
                    )
                elif hint.source:
                    # A source attribute in the specification overrides any make functions.
                    return hint.source(
                        self.faker,
                        **dependencies,
                    )
                elif hasattr(obj_type, f"make_{field}"):
                    # This is another way to specify custom generation logic.
                    return getattr(obj_type, f"make_{field}")(
                        self.faker,
                        **dependencies,
                    )
                else:
                    return self.build(hint.type)
        else:
            return self.build(hint)


class SpecialValue(enum.Enum):
    MISSING = enum.auto()


MISSING = SpecialValue.MISSING


@dataclasses.dataclass
class Specification:
    type: type
    correlation: str | None = None
    distribution: str = "default"
    source: collections.abc.Callable[[faker.proxy.Faker], any] | None = None
    dependson: tuple[str] = ()

    def __repr__(self):
        specs = (
            f"{attr}: {getattr(self, attr)}"
            for attr in ("correlation", "distribution", "dependson")
            # Print if the value is not None. Also ignore zero-length tuples.
            if getattr(self, attr) is not None
            and (not isinstance(getattr(self, attr), tuple) or getattr(self, attr))
        )
        return f"{self.type.__name__}({', '.join(specs)})"

    async def analyze(self, database_instance: DatabaseInstance, collection_name: str, field_name: str) -> None:
        """
        Runs 'analyze' on all fields in the Specification.

        If any field is a sub-Specification, run 'analyze' on the subfields too.
        """

        await database_instance.analyze_field(collection_name, field_name)

        if dataclasses.is_dataclass(self.type):
            for subfield in dataclasses.fields(self.type):
                if isinstance(subfield.type, Specification):
                    subfield_path = field_name + "." + subfield.name
                    await subfield.type.analyze(database_instance, collection_name, subfield_path)
