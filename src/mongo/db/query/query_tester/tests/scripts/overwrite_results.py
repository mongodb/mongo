"""
Given a set of query-correctness-tests directories, it finds all the .actual files that were generated from failing query_tester tests and replaces the corresponding .results with the .actual file. This allows for a quick way to git diff to view the diffs locally as well as for quick staging of acceptable changes using git add.
"""

import argparse
import fnmatch
import os
import shutil
from pathlib import Path
from typing import Iterable


def main(repo_paths: Iterable[Path]):
    """
    Convenience function to overwrite all .results with their corresponding .actual files.
    Args:
        clone_dir: Path to the parent directory of the query-correctness-tests-* dirs.
    """
    for repo_path in repo_paths:
        print(f"\nOverwriting results in {repo_path}")
        for root, _, files in os.walk(repo_path):
            # Find all files that end in *.actual.
            curr_actual_files = (Path(root) / f for f in fnmatch.filter(files, "*.actual"))
            for actual_file in curr_actual_files:
                # Replace the contents of the corresponding .results with the .actual but retain original .actual.
                results_file = actual_file.with_suffix(".results")
                print(f"Overwiting {results_file} with {actual_file}")
                shutil.copyfile(actual_file, results_file)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Overwrite the .results files with their corresponding .actual files for easier git diff and git add."
    )
    parser.add_argument(
        "repo_paths",
        nargs="+",
        type=Path,
        help="Path to the directories of the query-correctness-tests-* repo checkouts.",
    )
    args = parser.parse_args()
    repo_paths = args.repo_paths

    main(repo_paths)
