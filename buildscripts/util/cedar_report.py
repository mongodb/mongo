"""Cedar report."""

from dataclasses import dataclass
from typing import List, Union


@dataclass
class CedarMetric:
    """Structure that holds metrics for Cedar."""

    name: str
    type: str
    value: Union[int, float]
    user_submitted: bool = False

    def as_dict(self) -> dict:
        """Return dictionary representation."""
        return {
            "name": self.name,
            "type": self.type,
            "value": self.value,
            "user_submitted": self.user_submitted,
        }


@dataclass
class CedarTestReport:
    """Structure that holds test report for Cedar."""

    test_name: str
    thread_level: int
    metrics: List[CedarMetric]

    def as_dict(self) -> dict:
        """Return dictionary representation."""
        return {
            "info": {
                "test_name": self.test_name,
                "args": {
                    "thread_level": self.thread_level,
                },
            },
            "metrics": [metric.as_dict() for metric in self.metrics],
        }
