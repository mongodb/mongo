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

"""
Example collection spec to demonstrate various features of this data generator.

Models a potential Employee record document in an HR system.
"""

import collections
import dataclasses
import datetime
import enum
import math

import faker
import pymongo
from datagen.util import MISSING, Specification


class Track(enum.IntEnum):
    IC = enum.auto()
    Management = enum.auto()

    def __repr__(self):
        return self.name


@dataclasses.dataclass
class Title:
    name: Specification(
        str, correlation="title.level", source=lambda fkr: fkr.job()
    )  # Correlated with level.
    level: Specification(int, correlation="title.level")  # Skew low.
    track: Specification(Track, correlation="title.level")  # Correlated with level, inverse skew.

    @staticmethod
    def compute_level(fkr: faker.proxy.Faker) -> float:
        return fkr.random.triangular(1, 7, 1)

    @staticmethod
    def compute_track(fkr: faker.proxy.Faker, title_level: float) -> Track:
        management_weight = 0 if title_level < 2 else title_level
        ic_weight = 1.1 ** (9 - management_weight)
        return fkr.random_element(
            collections.OrderedDict([(Track.IC, ic_weight), (Track.Management, management_weight)])
        )

    @staticmethod
    def make_level(fkr: faker.proxy.Faker) -> int:
        return math.floor(Title.compute_level(fkr))

    @staticmethod
    def make_track(fkr: faker.proxy.Faker) -> Track:
        title_level = Title.compute_level(fkr)
        return Title.compute_track(fkr, title_level)


# The past year's worth of work activity.
@dataclasses.dataclass
class Activity:
    tickets: Specification(
        list[int], correlation="title.level"
    )  # Weekly number of tickets closed. Correlated with title level and inversely with title track.
    meetings: Specification(
        list[int], correlation="title.level"
    )  # Weekly hours spent in meetings. Correlated with title level and with title track.

    @staticmethod
    def make_meetings(fkr: faker.proxy.Faker) -> list[int]:
        title_level = Title.compute_level(fkr)
        title_track = Title.compute_track(fkr, title_level)
        max_meetings = title_level * title_track.value * 5
        return [
            fkr.random_int(math.floor(0.7 * max_meetings), math.floor(max_meetings))
            for _ in range(10)
        ]

    @staticmethod
    def make_tickets(fkr: faker.proxy.Faker) -> list[int]:
        title_level = Title.compute_level(fkr)
        title_track = Title.compute_track(fkr, title_level)
        max_tickets = title_level * (Track.Management.value * 1.05 - title_track.value) * 3
        return [
            fkr.random_int(math.floor(0.7 * max_tickets), math.floor(max_tickets))
            for _ in range(10)
        ]


@dataclasses.dataclass
class Employee:
    name: Specification(str, source=lambda fkr: fkr.name())  # Generically random.
    title: Specification(Title)
    start_date: Specification(
        datetime.date, correlation="title.level", dependson=("title",)
    )  # Loosely inversely correlated with title level.
    team: Specification(str, source=lambda fkr: fkr.bs().title())  # Uniformly random.
    salary: Specification(
        int, correlation="title.level", dependson=("team",)
    )  # Correlated with title level & team.
    activity: Specification(Activity)  # Correlated with title level.
    mark: Specification(
        int,
        source=lambda fkr: MISSING if fkr.random.random() < 0.5 else fkr.random_int(0, 1000000000),
    )

    @staticmethod
    def compute_salary(fkr: faker.proxy.Faker, title_level: float, team_value: int) -> int:
        band_min = title_level**1.1 * team_value * 10000
        band_max = title_level**1.2 * team_value * 15000
        return fkr.random_int(math.floor(band_min), math.ceil(band_max))

    @staticmethod
    def make_salary(fkr: faker.proxy.Faker, team: str) -> int:
        title_level = Title.compute_level(fkr)
        team_value = 5 + abs(hash(team) % 5)
        return Employee.compute_salary(fkr, title_level, team_value)

    @staticmethod
    def make_start_date(fkr: faker.proxy.Faker, title: Title) -> datetime.datetime:
        today = datetime.datetime.today()
        ago = datetime.timedelta(
            days=fkr.random_int(title.level * 250, title.level * (title.level + 2) * 110)
        )
        return today - ago


# Using a function to return some indices.
def index_set_1() -> list[pymongo.IndexModel]:
    return [
        pymongo.IndexModel(keys="title.level", name="title_level"),
        pymongo.IndexModel(
            keys=[("title.level", pymongo.DESCENDING), ("salary", pymongo.DESCENDING)],
            name="level_and_salary",
        ),
    ]


# Using a global to define some indices.
index_set_2 = [
    pymongo.IndexModel(
        keys=[("title.level", pymongo.ASCENDING), ("start_date", pymongo.DESCENDING)],
        name="level_and_start",
    ),
    pymongo.IndexModel(keys="activity.tickets", name="weekly_tickets"),
]
