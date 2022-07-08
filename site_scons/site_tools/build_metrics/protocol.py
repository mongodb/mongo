from typing import Tuple
from typing_extensions import Protocol
from abc import abstractmethod


class BuildMetricsCollector(Protocol):
    @abstractmethod
    def finalize(self) -> Tuple[str, str]:
        raise NotImplementedError
