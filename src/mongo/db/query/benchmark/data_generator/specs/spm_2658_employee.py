# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0

"""
Models a potential Employee record document in an HR system.

This specification is intended to demo basic capabilities of data generator, in particular, how
correlated data can be generated via both the internal linear correlation RNG and also custom
functions.
"""

import collections
import dataclasses
import datetime
import enum
import math

import pymongo
from datagen.distribution import *
from datagen.random import global_faker
from datagen.util import MISSING, Specification


class Track(enum.IntEnum):
    IC = enum.auto()
    Management = enum.auto()

    def __repr__(self):
        return self.name


@dataclasses.dataclass
class Title:
    @staticmethod
    def compute_level() -> float:
        return global_faker().random.triangular(1, 7, 1)

    @staticmethod
    def compute_track(title_level: float) -> Track:
        management_weight = 0 if title_level < 2 else title_level
        ic_weight = 1.1 ** (9 - management_weight)
        return global_faker().random_element(
            collections.OrderedDict([(Track.IC, ic_weight), (Track.Management, management_weight)])
        )

    @staticmethod
    def make_level() -> int:
        return math.floor(Title.compute_level())

    @staticmethod
    def make_track(level: int) -> Track:
        return Title.compute_track(level)

    name: Specification(
        correlation(lambda: global_faker().job(), "title.level")
    )  # Correlated with level.
    level: Specification(correlation(make_level, "title.level"))  # Skew low.
    track: Specification(
        correlation(make_track, "title.level"), dependson=("level",)
    )  # Correlated with level, inverse skew.


# The past year's worth of work activity.
@dataclasses.dataclass
class Activity:
    @staticmethod
    def make_meetings(title) -> int:
        max_meetings = title.level * title.track * 5
        return global_faker().random_int(math.floor(0.7 * max_meetings), math.floor(max_meetings))

    @staticmethod
    def make_tickets(title) -> int:
        max_tickets = title.level * (Track.Management.value * 1.05 - title.track.value) * 3
        return global_faker().random_int(math.floor(0.7 * max_tickets), math.floor(max_tickets))

    tickets: Specification(
        array(10, make_tickets)
    )  # Weekly number of tickets closed. Correlated with title level and inversely with title track.
    meetings: Specification(
        array(10, make_meetings)
    )  # Weekly hours spent in meetings. Correlated with title level and with title track.


def make_salary(title: Title, team: str) -> int:
    team_value = 5 + abs(ord(team[-1]) % 5)
    return Employee.compute_salary(title.level, team_value)


DAY = datetime.datetime.strptime("21/11/06 16:30", "%d/%m/%y %H:%M")


def make_start_date(title: Title) -> datetime.date:
    ago = datetime.timedelta(
        days=global_faker().random_int(title.level * 250, title.level * (title.level + 2) * 110)
    )
    return DAY - ago


@dataclasses.dataclass
class Employee:
    name: Specification(lambda: global_faker().name())  # Generically random.
    title: Title
    start_date: Specification(
        source=make_start_date, dependson=("title",)
    )  # Loosely inversely correlated with title level.
    team: Specification(source=lambda: global_faker().bs().title())  # Uniformly random.
    salary: Specification(
        source=make_salary,
        dependson=(
            "title",
            "team",
        ),
    )  # Correlated with title level & team.
    activity: Specification(Activity, dependson=("title",))  # Correlated with title level.
    mark: Specification(
        uniform([MISSING, lambda: global_faker().random_int(0, 1000000000)]),
    )

    @staticmethod
    def compute_salary(title_level: float, team_value: int) -> int:
        band_min = title_level**1.1 * team_value * 10000
        band_max = title_level**1.2 * team_value * 15000
        return global_faker().random_int(math.floor(band_min), math.ceil(band_max))


# Using a function to return some indexes.
def index_set_1() -> list[pymongo.IndexModel]:
    return [
        pymongo.IndexModel(keys="title.level", name="title_level"),
        pymongo.IndexModel(
            keys=[("title.level", pymongo.DESCENDING), ("salary", pymongo.DESCENDING)],
            name="level_and_salary",
        ),
    ]


# Using a global to define some indexes.
index_set_2 = [
    pymongo.IndexModel(
        keys=[("title.level", pymongo.ASCENDING), ("start_date", pymongo.DESCENDING)],
        name="level_and_start",
    ),
    pymongo.IndexModel(keys="activity.tickets", name="weekly_tickets"),
]
