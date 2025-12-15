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

import dataclasses
import enum
import typing

import datagen.statistics
from datagen.database_instance import DatabaseInstance
from datagen.random import global_faker

STATISTICS = datagen.statistics.StatisticsRegister()


class SpecialValue(enum.Enum):
    MISSING = enum.auto()


MISSING = SpecialValue.MISSING


@dataclasses.dataclass
class Specification:
    """Specifies a distribution function for a field
    source: The distribution function.
    dependson: The fields from the same object that the values of this field depends on.
    """

    def __init__(self, source: any, dependson=()):
        self.source = _distributionify(source)
        self.orig_source = source
        self.dependson = dependson

    async def analyze(
        self, database_instance: DatabaseInstance, collection_name: str, field_name: str
    ) -> None:
        """
        Runs 'analyze' on all fields in the Specification.

        If any field is a sub-Specification, run 'analyze' on the subfields too.
        """

        await database_instance.analyze_field(collection_name, field_name)

        if dataclasses.is_dataclass(self.orig_source):
            for subfield in dataclasses.fields(self.orig_source):
                if isinstance(subfield.type, Specification):
                    subfield_path = field_name + "." + subfield.name
                    await subfield.type.analyze(database_instance, collection_name, subfield_path)


def _constant(val: any) -> callable:
    def result_func(**kwargs):
        return val

    return result_func


def _distributionify(input: any) -> callable:
    from inspect import isclass

    if isclass(input):
        hints = typing.get_type_hints(input)
        if hints:
            # Reorder the fields so that fields that depend on other fields are only constructed
            # after those fields are built.
            queue = []
            reordered = []
            already_reordered = set()

            def queue_items():
                for k, v in hints.items():
                    yield k, v
                for k, v in queue:
                    yield k, v

            for field, hint in queue_items():
                if isinstance(hint, Specification):
                    if hint.dependson and not isinstance(hint.dependson, tuple):
                        raise RuntimeError(f"`dependson` argument for field {field} is not a tuple")
                    dependencies_not_fulfilled = [
                        d for d in hint.dependson if d not in already_reordered
                    ]
                    if dependencies_not_fulfilled:
                        nonexistent_dependencies = [
                            d for d in dependencies_not_fulfilled if d not in hints
                        ]
                        if nonexistent_dependencies:
                            raise RuntimeError(
                                f"Field {field} depends on nonexistence field(s) {str(nonexistent_dependencies)}"
                            )
                        else:
                            queue.append((field, hint))
                    else:
                        reordered.append((field, hint.dependson, _distributionify(hint.source)))
                        already_reordered.add(field)
                else:
                    reordered.append((field, (), _distributionify(hint)))
                    already_reordered.add(field)

            def result_func(**dependencies):
                fields = {}
                for field, dependson, source in reordered:
                    with STATISTICS.path_cm(field):
                        new_dependencies = {
                            dependency: fields[dependency] for dependency in dependson
                        }
                        new_dependencies.update(dependencies)
                        fields[field] = source(**new_dependencies)
                STATISTICS.register_fields(fields)
                return input(**fields)

            return result_func
        # From here, generate some basic types by default.
        elif issubclass(input, enum.Enum):
            return lambda **kwargs: global_faker().enum(input)
        elif hasattr(input, "__args__"):
            value_types = input.__args__

            return lambda **kwargs: getattr(global_faker(), f"py{input.__name__}")(value_types)
        else:
            return lambda **kwargs: getattr(global_faker(), f"py{input.__name__}")()

    elif not callable(input):
        return _constant(input)
    else:
        return input
