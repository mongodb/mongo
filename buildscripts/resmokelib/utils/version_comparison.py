"""
Compare MongoDB bin/FCV version strings to match MongoRunner.compareBinVersions in the mongo shell.

Used by resmoke hooks (e.g. add_remove_shards) that need the same version comparison semantics
as jstests (e.g. jstests/sharding/libs/remove_shard_util.js).
"""


def compare_bin_versions(version_a: str, version_b: str) -> int:
    """
    Compare two version strings (e.g. FCV or bin version) and return:

        - 1 if version_a is more recent than version_b
        - 0 if they are equal (up to the length of the shorter version)
        - -1 if version_a is older than version_b

    Matches MongoRunner.compareBinVersions in src/mongo/shell/servers.js:
    - Compares only up to the length of the shorter version (e.g. "8.3" and "8.3.0" compare equal).
    - Version strings are split by ".", and the last component may contain a "-" (githash),
      which is treated as a separate element.
    - Does not resolve symbolic versions like "latest" or "last-lts"; pass concrete versions
      (e.g. from admin.system.version or serverStatus).

    :param version_a: First version string (e.g. "8.3", "8.3.0").
    :param version_b: Second version string (e.g. "8.3").
    :return: 1, 0, or -1.
    """
    a_parts = _version_string_to_parts(version_a)
    b_parts = _version_string_to_parts(version_b)
    for elem_a, elem_b in zip(a_parts, b_parts):
        if elem_a == elem_b:
            continue
        try:
            num_a = int(elem_a)
            num_b = int(elem_b)
        except ValueError as e:
            raise ValueError(
                f"Cannot compare non-equal non-numeric versions: {elem_a!r}, {elem_b!r} "
                f"(from {version_a!r} vs {version_b!r})"
            ) from e
        if num_a > num_b:
            return 1
        if num_a < num_b:
            return -1
    return 0


def _version_string_to_parts(version_str: str) -> list:
    """Split a version string into comparable parts (dot-separated, then last part split on '-')."""
    version_str = (version_str or "").strip()
    if not version_str:
        raise ValueError("Version strings must not be empty")
    parts = version_str.split(".")
    if len(parts) < 2:
        raise ValueError(
            f"MongoDB versions must have at least two components to compare, "
            f'but "{version_str}" has {len(parts)}'
        )
    # Treat githash as a separate element if present (e.g. "8.3.0-rc0" -> ..., "0", "rc0").
    last = parts.pop()
    parts.extend(last.split("-"))
    return parts
