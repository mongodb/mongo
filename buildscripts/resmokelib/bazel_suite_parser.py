"""Parser for BUILD.bazel files to extract resmoke_suite_test configuration.

This module parses BUILD.bazel files without invoking bazel, supporting a simplified
subset of Bazel syntax:
- Simple lists of targets (no select() expressions)
- Direct file targets (e.g., "//jstests/foo:bar.js")
- all_javascript_files targets (globs *.js in directory)
- all_subpackage_javascript_files targets (recursively includes all JS from subpackages)
"""

import functools
import os
import re
from typing import Dict, List


class BazelParseError(Exception):
    """Exception raised when parsing BUILD.bazel files fails."""

    pass


@functools.cache
def parse_resmoke_suite_test(target_label: str) -> Dict[str, List[str]]:
    """Parse a resmoke_suite_test target from BUILD.bazel.
    Args:
        target_label: Bazel target label like "//buildscripts/resmokeconfig:core"
    Returns:
        Dictionary with extracted attributes:
            - srcs: List of test file labels
            - exclude_files: List of test file labels to exclude
            - exclude_with_any_tags: List of tag strings
            - include_with_any_tags: List of tag strings
            - group_size: Integer or None for number of tests per group (for test_kind: parallel_fsm_workload_test)
            - group_count_multiplier: String for group count multiplier (for test_kind: parallel_fsm_workload_test)
    Raises:
        BazelParseError: If BUILD.bazel file not found or target not found
    """
    package, target_name = _parse_label(target_label)
    build_file = os.path.join(package, "BUILD.bazel")

    if not os.path.exists(build_file):
        raise BazelParseError(
            f"BUILD.bazel file not found at '{build_file}' for target '{target_label}'"
        )
    with open(build_file, "r") as f:
        content = f.read()

    # Find the resmoke_suite_test block
    # Pattern matches: resmoke_suite_test(name = "target_name", ...)
    pattern = r'resmoke_suite_test\s*\(\s*name\s*=\s*["\']' + re.escape(target_name) + r'["\']'
    match = re.search(pattern, content)
    if not match:
        raise BazelParseError(
            f"Target '{target_name}' not found in '{build_file}'. "
            f'Expected a resmoke_suite_test rule with name = "{target_name}"'
        )

    # Extract the rule block by finding balanced parentheses
    rule_start = match.start()
    paren_start = content.index("(", rule_start)
    paren_count = 0
    rule_end = paren_start
    for i in range(paren_start, len(content)):
        if content[i] == "(":
            paren_count += 1
        elif content[i] == ")":
            paren_count -= 1
            if paren_count == 0:
                rule_end = i + 1
                break

    if paren_count != 0:
        raise BazelParseError(
            f"Unbalanced parentheses in resmoke_suite_test definition for '{target_label}'"
        )
    rule_block = content[rule_start:rule_end]

    return {
        "srcs": _extract_attribute(rule_block, "srcs"),
        "exclude_files": _extract_attribute(rule_block, "exclude_files"),
        "exclude_with_any_tags": _extract_attribute(rule_block, "exclude_with_any_tags"),
        "include_with_any_tags": _extract_attribute(rule_block, "include_with_any_tags"),
        "group_size": _extract_int_attribute(rule_block, "group_size"),
        "group_count_multiplier": _extract_scalar_attribute(rule_block, "group_count_multiplier"),
    }


def _parse_label(target_label: str) -> tuple[str, str]:
    """Parse a Bazel target label into package path and target name.
    Args:
        target_label: A Bazel target label like "//package/path:target_name"
    Returns:
        Tuple of (package_path, target_name)
    Raises:
        BazelParseError: If the label format is invalid
    """
    if not target_label.startswith("//"):
        raise BazelParseError(
            f"Unsupported Bazel target label '{target_label}': must start with '//'"
        )
    # Remove leading "//"
    label_without_prefix = target_label[2:]

    # Split on ":"
    if ":" not in label_without_prefix:
        raise BazelParseError(
            f"Unsupported Bazel target label '{target_label}': must contain ':' separator"
        )
    package, target_name = label_without_prefix.split(":", 1)

    return package, target_name


def _extract_attribute(block: str, attribute_name: str) -> List[str]:
    """Extract an attribute from a BUILD.bazel rule block.
    Args:
        block: The text content of a BUILD.bazel rule block
        attribute_name: The name of the attribute to extract (e.g., "srcs")
    Returns:
        List of string values from the attribute. Returns empty list if attribute not found.
    """
    # Pattern to match: attribute_name = [...]
    # This handles multiline lists and ignores comments
    pattern = rf"{attribute_name}\s*=\s*\[(.*?)\]"
    match = re.search(pattern, block, re.DOTALL)
    if not match:
        return []
    list_content = match.group(1)

    # Extract quoted strings, handling both single and double quotes
    # This pattern finds strings in quotes, ignoring comments
    items = []
    for line in list_content.split("\n"):
        # Remove inline comments
        line = re.sub(r"#.*$", "", line)

        # Find all quoted strings in the line
        string_pattern = r'["\']([^"\']+)["\']'
        items.extend(re.findall(string_pattern, line))

    return items


def _extract_int_attribute(block: str, attribute_name: str) -> int | None:
    """Extract an integer attribute from a BUILD.bazel rule block.
    Args:
        block: The text content of a BUILD.bazel rule block
        attribute_name: The name of the attribute to extract (e.g., "group_size")
    Returns:
        Integer value of the attribute. Returns None if attribute not found.
    """
    # Pattern to match: attribute_name = <integer>
    pattern = rf"{attribute_name}\s*=\s*(\d+)"
    match = re.search(pattern, block)
    if not match:
        return None
    return int(match.group(1))


def _extract_scalar_attribute(block: str, attribute_name: str) -> str:
    """Extract a string attribute from a BUILD.bazel rule block.
    Args:
        block: The text content of a BUILD.bazel rule block
        attribute_name: The name of the attribute to extract (e.g., "group_count_multiplier")
    Returns:
        String value of the attribute. Returns empty string if attribute not found.
    """
    # Pattern to match: attribute_name = "<value>"
    quoted_pattern = rf'{attribute_name}\s*=\s*["\']([^"\']+)["\']'
    match = re.search(quoted_pattern, block)
    if match:
        return match.group(1)

    return ""


def resolve_target_to_files(target_label: str) -> str:
    """Resolve a Bazel target label to glob patterns or file paths.
    Supported target types:
    - Direct file: "//jstests/foo:bar.js" → "jstests/foo/bar.js"
    - all_javascript_files: returns glob pattern "package/*.js"
    - all_subpackage_javascript_files: returns glob pattern "package/**/*.js"
    Args:
        target_label: Bazel target label to resolve
    Returns:
        File path or glob pattern (relative to repo root)
    Raises:
        BazelParseError: If target type is unsupported
    """
    package, target_name = _parse_label(target_label)

    if target_name.endswith(".js"):
        # Direct file reference
        return os.path.join(package, target_name)

    elif target_name == "all_javascript_files":
        # Return glob pattern for *.js in package directory
        return os.path.join(package, "*.js")

    elif target_name == "all_subpackage_javascript_files":
        # Return glob pattern for recursive **/*.js
        return os.path.join(package, "**/*.js")

    else:
        raise BazelParseError(
            f"Unsupported target type '{target_label}'. "
            f"Supported types: direct .js files, all_javascript_files, all_subpackage_javascript_files"
        )
