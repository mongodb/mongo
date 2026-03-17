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

from buildscripts.resmokelib import config


class BazelParseError(Exception):
    """Exception raised when parsing BUILD.bazel files fails."""

    pass


@functools.cache
def parse_resmoke_suite_test(target_label: str) -> dict[str, list[str]]:
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

    # Parse load statements to build identifier -> .bzl file mapping
    identifier_to_bzl_file = _parse_load_statements(content, package)

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
        "srcs": _extract_attribute(rule_block, "srcs", identifier_to_bzl_file, build_file),
        "exclude_files": _extract_attribute(
            rule_block, "exclude_files", identifier_to_bzl_file, build_file
        ),
        "exclude_with_any_tags": _extract_attribute(
            rule_block, "exclude_with_any_tags", identifier_to_bzl_file, build_file
        ),
        "include_with_any_tags": _extract_attribute(
            rule_block, "include_with_any_tags", identifier_to_bzl_file, build_file
        ),
        "group_size": _extract_int_attribute(rule_block, "group_size"),
        "group_count_multiplier": _extract_scalar_attribute(rule_block, "group_count_multiplier"),
    }


def _parse_label(target_label: str) -> tuple[str, str]:
    """Parse a Bazel target label into package path and target name.
    Args:
        target_label: A Bazel target label like "//package/path:target_name"
    Returns:
        Tuple of (absolute_package_path, target_name) where absolute_package_path
        is the full path to the package directory (for finding BUILD.bazel files).
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

    # Return absolute path for finding BUILD.bazel files
    return os.path.join(config.RESMOKE_ROOT, package), target_name


def _parse_load_statements(content: str, package: str) -> dict[str, str]:
    """Parse load statements from BUILD.bazel content.

    Extracts identifier to .bzl file mappings from load statements.
    Example: load("//path/to:file.bzl", "identifier1", "identifier2")

    Args:
        content: The BUILD.bazel file content
        package: The package path of the BUILD.bazel file

    Returns:
        Dictionary mapping identifier names to absolute .bzl file paths
    """
    identifier_to_bzl_file = {}

    # Find all load statements
    for line in content.split("\n"):
        if not line.strip().startswith("load("):
            continue

        # Extract the full load statement (may span multiple lines)
        # Looking for load("//path/to:file.bzl", "identifier1", "identifier2", ...)
        match = re.match(r'load\s*\(\s*["\']([^"\']+)["\'](.+?)\)', line)
        if match:
            bzl_label = match.group(1)
            identifiers_str = match.group(2)

            # Convert the .bzl label to a file path
            # Example: "//jstests/suites:selectors.bzl"
            #       -> "/absolute/path/to/jstests/suites/selectors.bzl"
            if bzl_label.startswith("//"):
                # Absolute Bazel label - resolve relative to RESMOKE_ROOT
                relative_path = bzl_label[2:].replace(":", "/")
                bzl_path = os.path.normpath(os.path.join(config.RESMOKE_ROOT, relative_path))
            else:
                # Relative path - resolve relative to current package
                # Strip leading ':' if present (package-relative reference)
                relative_part = bzl_label[1:] if bzl_label.startswith(":") else bzl_label
                bzl_path = os.path.normpath(os.path.join(package, relative_part.replace(":", "/")))

            # Extract all identifiers from the load statement
            identifier_pattern = r'["\']([^"\']+)["\']'
            identifiers = re.findall(identifier_pattern, identifiers_str)

            # Map each identifier to the .bzl file
            for identifier in identifiers:
                identifier_to_bzl_file[identifier] = bzl_path

    return identifier_to_bzl_file


def _extract_attribute(
    block: str,
    attribute_name: str,
    identifier_to_bzl_file: dict[str, str] = None,
    build_file: str = None,
) -> list[str]:
    """Extract an attribute from a BUILD.bazel rule block.

    Supports simple lists and list concatenation with identifiers:
    - Simple list: srcs = ["file1.js", "file2.js"]
    - List concatenation: srcs = ["file1.js"] + some_identifier

    Args:
        block: The text content of a BUILD.bazel rule block
        attribute_name: The name of the attribute to extract (e.g., "srcs")
        identifier_to_bzl_file: Dictionary mapping identifier names to .bzl file paths
        build_file: Path to the BUILD.bazel file for resolving local identifiers
    Returns:
        List of string values from the attribute. Returns empty list if attribute not found.
    """
    if identifier_to_bzl_file is None:
        identifier_to_bzl_file = {}

    # Pattern to match: attribute_name = <expression>
    # Captures everything until comma + newline + next attribute/paren, or end of string
    pattern = rf"{attribute_name}\s*=\s*(.+?)(?=,\s*\n\s*(?:\w+\s*=|\))|\Z)"
    match = re.search(pattern, block, re.DOTALL | re.MULTILINE)
    if not match:
        return []

    expression = match.group(1).strip()

    # Split expression by '+' operator to handle concatenation
    items = []
    parts = re.split(r"\+", expression)

    for part in parts:
        part = part.strip()

        # Check if this part is a list literal
        if part.startswith("[") and part.endswith("]"):
            # Extract the content between brackets
            list_content = part[1:-1]

            # Extract quoted strings, handling both single and double quotes
            for line in list_content.split("\n"):
                # Remove inline comments
                line = re.sub(r"#.*$", "", line)

                # Find all quoted strings in the line
                string_pattern = r'["\']([^"\']+)["\']'
                items.extend(re.findall(string_pattern, line))

        # Check if this part is an identifier (not a list literal)
        elif re.match(r"^[a-zA-Z_][a-zA-Z0-9_]*$", part):
            # Resolve the identifier to a list of labels
            resolved_items = _resolve_identifier_to_labels(part, identifier_to_bzl_file, build_file)
            items.extend(resolved_items)

    return items


def _resolve_identifier_to_labels(
    identifier: str, identifier_to_bzl_file: dict[str, str], build_file: str = None
) -> list[str]:
    """Convert a Bazel identifier to a list of labels.

    This function resolves identifiers used in list concatenation expressions.
    For example, in: srcs = ["file.js"] + sharding_jscore_passthrough_srcs
    The identifier 'sharding_jscore_passthrough_srcs' would be resolved to its
    corresponding list of labels by reading its definition from the .bzl file.

    If the identifier is not found in load statements, this function will attempt
    to find it defined in the BUILD.bazel file itself.

    Args:
        identifier: The identifier name to resolve (e.g., "sharding_jscore_passthrough_srcs")
        identifier_to_bzl_file: Dictionary mapping identifier names to .bzl file paths
        build_file: Path to the BUILD.bazel file for resolving local identifiers

    Returns:
        List of resolved label strings
    """
    identifier_pattern = rf"^{re.escape(identifier)}\s*=\s*\[(.+?)\]"

    if identifier in identifier_to_bzl_file:
        bzl_file_path = identifier_to_bzl_file[identifier]
        with open(bzl_file_path, "r") as f:
            bzl_content = f.read()

        # Find the identifier definition in the .bzl file
        match = re.search(identifier_pattern, bzl_content, re.MULTILINE | re.DOTALL)
        if not match:
            raise BazelParseError(
                f"Could not find definition of identifier '{identifier}' in '{bzl_file_path}'"
            )
    else:
        # Try to find the identifier in the BUILD.bazel file itself
        with open(build_file, "r") as f:
            build_content = f.read()

        # Look for identifier definition in BUILD.bazel
        match = re.search(identifier_pattern, build_content, re.MULTILINE | re.DOTALL)
        if not match:
            raise BazelParseError(
                f"Identifier '{identifier}' referenced but not found in load statements "
                f"or in BUILD.bazel file."
            )

    # Extract all quoted strings from the list
    items = []
    for line in match.group(1).split("\n"):
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
    absolute_package, target_name = _parse_label(target_label)

    # Convert absolute package path to relative (for suite configuration)
    relative_package = os.path.relpath(absolute_package, config.RESMOKE_ROOT)

    if target_name.endswith(".js"):
        # Direct file reference
        return os.path.join(relative_package, target_name)

    elif target_name == "all_javascript_files":
        # Return glob pattern for *.js in package directory
        return os.path.join(relative_package, "*.js")

    elif target_name == "all_subpackage_javascript_files":
        # Return glob pattern for recursive **/*.js
        return os.path.join(relative_package, "**/*.js")

    else:
        raise BazelParseError(
            f"Unsupported target type '{target_label}'. "
            f"Supported types: direct .js files, all_javascript_files, all_subpackage_javascript_files"
        )
