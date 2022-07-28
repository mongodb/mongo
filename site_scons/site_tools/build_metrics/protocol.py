from typing import Tuple, Any
from typing_extensions import Protocol
from abc import abstractmethod


class BuildMetricsCollector(Protocol):
    @abstractmethod
    def finalize(self) -> Tuple[str, Any]:
        raise NotImplementedError

    @abstractmethod
    def get_name() -> str:
        raise NotImplementedError
