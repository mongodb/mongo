import dataclasses

import faker
from datagen.util import Specification


@dataclasses.dataclass
class Uncorrelated:
    field1: Specification(str)
    field2: Specification(str)

    @staticmethod
    def make_field1(fkr: faker.proxy.Faker) -> int:
        return fkr.random.choice(['a','b'])

    @staticmethod
    def make_field2(fkr: faker.proxy.Faker) -> int:
        return fkr.random.choice(['a','b'])
