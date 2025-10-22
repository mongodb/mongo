import os
import re
from typing import Optional, Set

MONGO_DOCUMENT_SOURCE_DIR = "src/mongo/db/pipeline"

# Regex to find bool requiresAuthzChecks() const override { return false; }
AUTHZ_OPT_OUT_PATTERN = re.compile(
    r"bool\s+requiresAuthzChecks\s*\(\s*\)\s*const\s*override\s*\{\s*return\s+false;\s*\}",
    re.DOTALL,
)

# Regex to find the registration macro and capture the stage name
STAGE_NAME_PATTERN = re.compile(
    r"REGISTER(?:_INTERNAL|_TEST)?_DOCUMENT_SOURCE(?:_WITH_FEATURE_FLAG)?\("
    r"\s*(\w+)",  # Capture the stage name (e.g., "listSessions")
    re.IGNORECASE,
)


def read_file(filepath: str) -> Optional[str]:
    try:
        if os.path.exists(filepath):
            with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
                return f.read()
    except IOError as e:
        print(f"Could not read {filepath}: {e}")
    return None


def check_file_pair(base_path: str) -> Optional[str]:
    """
    Check a .h/.cpp file pair for opt-out pattern and stage name.
    Returns the stage name if found, None otherwise.
    """
    header_path = f"{base_path}.h"
    cpp_path = f"{base_path}.cpp"

    header_content = read_file(header_path)
    cpp_content = read_file(cpp_path)

    opt_out_found = False
    if (header_content and AUTHZ_OPT_OUT_PATTERN.search(header_content)) or (
        cpp_content and AUTHZ_OPT_OUT_PATTERN.search(cpp_content)
    ):
        opt_out_found = True

    if not opt_out_found:
        return None

    for content in [cpp_content, header_content]:
        if content:
            match = STAGE_NAME_PATTERN.search(content)
            if match:
                return f"${match.group(1)}"

    # Opt-out found but no stage name
    print(
        f"Opt-out found in '{os.path.basename(base_path)}' "
        "but registration macro not found in .h or .cpp file."
    )
    return None


def find_authz_opt_out_stages() -> Set[str]:
    """
    Scans the pipeline directory to find aggregation stages that
    opted out of authorization checks.

    Returns a set of stage names that opted out.
    """
    if not os.path.isdir(MONGO_DOCUMENT_SOURCE_DIR):
        print(f"Error: Directory '{MONGO_DOCUMENT_SOURCE_DIR}' does not exist")
        return set()

    print(f"Scanning for aggregation stages in '{MONGO_DOCUMENT_SOURCE_DIR}'")
    found_stages: Set[str] = set()
    processed_bases: Set[str] = set()

    # Walk through directory
    for root, _, files in os.walk(MONGO_DOCUMENT_SOURCE_DIR):
        for filename in files:
            if not filename.endswith((".h", ".cpp")):
                continue

            filepath = os.path.join(root, filename)
            base_path = os.path.splitext(filepath)[0]

            # Skip if already processed this file pair
            if base_path in processed_bases:
                continue

            processed_bases.add(base_path)

            stage_name = check_file_pair(base_path)
            if stage_name:
                found_stages.add(stage_name)

    return found_stages


def main():
    found_stages = find_authz_opt_out_stages()

    if found_stages:
        print(f"\nFound {len(found_stages)} stage(s) that opted out of authz checks:")
        for stage in sorted(found_stages):
            print(f"  - {stage}")
    else:
        print("\nNo aggregation stages found that explicitly opted out.")


if __name__ == "__main__":
    main()
