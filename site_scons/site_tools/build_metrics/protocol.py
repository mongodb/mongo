from typing_extensions import Protocol
from abc import abstractmethod


class BuildMetricsCollector(Protocol):
    @abstractmethod
    def finalize(self):
        raise NotImplementedError
