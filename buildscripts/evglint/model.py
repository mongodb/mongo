"""Type annotations for evglint."""
from typing import Callable, List

LintRule = Callable[[dict], List["LintError"]]
LintError = str
