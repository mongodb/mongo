import collections
import dataclasses

import faker
from datagen.util import Specification, uncorrelated_faker


@dataclasses.dataclass
class PartiallyCorrelated:
    field1: Specification(str, correlation="correlation1")
    field2: Specification(str, correlation="correlation1")

    @staticmethod
    def make_field1(fkr: faker.proxy.Faker) -> int:
        return fkr.random.choice(['a','b'])

    @staticmethod
    def make_field2(fkr: faker.proxy.Faker) -> str:
        return fkr.random_element(
            collections.OrderedDict([
                (uncorrelated_faker().random.choice(['c','d']), 0.1),
                (fkr.random.choice(['a','b']), 0.9)
            ])
        )
