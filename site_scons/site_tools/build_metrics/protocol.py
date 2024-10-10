from abc import abstractmethod
from typing import Any, Tuple

from typing_extensions import Protocol


class BuildMetricsCollector(Protocol):
    @abstractmethod
    def finalize(self) -> Tuple[str, Any]:
        raise NotImplementedError

    @abstractmethod
    def get_name() -> str:
        raise NotImplementedError
