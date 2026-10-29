"""Parse patch-local test bypasses and keep selection bounded by suite candidates."""

import json

BYPASS_PARAMETER = "test_selection_bypass_quarantine"
BYPASS_ENVIRONMENT = "TEST_SELECTION_BYPASS_QUARANTINE"


def parse_bypass_tests(raw: str) -> set[str]:
    """Decode exact test names, rejecting malformed bypasses rather than ignoring them."""
    if raw == "":
        return set()
    error = f"{BYPASS_PARAMETER} must be a JSON array of nonempty strings"
    if not isinstance(raw, str):
        raise ValueError(error)
    try:
        names = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ValueError(error) from exc
    if not isinstance(names, list) or any(not isinstance(name, str) or not name for name in names):
        raise ValueError(error)
    return set(names)


def merge_selected_tests(
    candidates: list[str], selected: list[str], bypass: set[str]
) -> tuple[list[str], list[str]]:
    """Filter candidates in their original order, preserving candidate duplicates."""
    allowed = set(selected) | bypass
    return (
        [test for test in candidates if test in allowed],
        [test for test in candidates if test not in allowed],
    )
