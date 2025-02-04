"""
Utility module containing helper functions for error handling, path validation,
and directory validation. These functions are specifically tailored to facilitate
the use of the feature extractor tool within the QueryTester framework.

Functions include:
- Error handling to ensure proper script execution.
- Path validation to check the existence and correctness of file paths.
- Directory validation to check the existence of directory paths.
"""

import argparse
import os
import sys
from pathlib import Path
from typing import Optional, Tuple

MONGODB_URI = "mongodb://127.0.0.1:27017/"


def clean_up_file(file_path: str, desc: str) -> None:
    """Deletes a file and prints an error if it doesn't exist."""
    path = Path(file_path)
    if path.is_file():
        path.unlink()
    else:
        print_and_exit(f"Error: {desc} {file_path} does not exist for cleanup.")


def construct_filenames(output_prefix: str, suffix: Optional[str] = None) -> Tuple[str, str]:
    """Constructs standardized filenames based on the prefix and optional suffix."""
    pkl_file = f"{output_prefix}.pkl"
    json_file = f"{output_prefix}_{suffix}.json" if suffix else f"{output_prefix}.json"
    return pkl_file, json_file


def extract_db(fail_filepath: Path) -> str:
    """Extracts DB name from a .fail file."""
    with fail_filepath.open(encoding="utf-8") as fail_file:
        lines = [line.strip() for line in fail_file if not line.startswith("//")]
        db = lines[1]
    return db


def move_file(src: str, dest: str) -> None:
    """Moves a file and handles exceptions."""
    try:
        src_path = Path(src)
        dest_path = Path(dest)
        src_path.rename(dest_path)
    except FileNotFoundError:
        print_and_exit(f"Error: File {src} not found for moving.")
    except OSError as e:
        print_and_exit(f"Error moving file {src} to {dest}: {e}")


def parse_args_common(desc: str, fail_filepath: bool = False) -> argparse.Namespace:
    """Helper function to parse common arguments, with optional additional arguments."""
    parser = argparse.ArgumentParser(description=desc)
    parser.add_argument(
        "mongo_repo_root", type=str, help="Path to the MongoDB repository root directory"
    )
    parser.add_argument(
        "feature_extractor_dir", type=str, help="Path to the feature-extractor directory"
    )
    parser.add_argument("output_prefix", type=str, help="Prefix for output files")
    if fail_filepath:
        parser.add_argument(
            "fail_filepath", type=Path, help="Path to the tester file with '.fail' extension"
        )
    return parser.parse_args()


def print_and_exit(msg: str) -> None:
    """Prints an error message and exits the program with status code 1."""
    print(msg)
    sys.exit(1)


def validate_and_change_directory(feature_extractor_dir: str) -> Tuple[Path, Path]:
    """Validates and changes to feature-extractor directory."""

    # Validate feature-extractor directory and change to it
    feature_extractor_path = validate_directory(feature_extractor_dir)
    os.chdir(os.path.expanduser(feature_extractor_path))

    return feature_extractor_path


def validate_directory(dirpath: str) -> Path:
    """Checks if the specified directory exists and exits if not."""
    path = Path(dirpath)
    if not path.is_dir():
        print_and_exit(f"Error: {path.stem} directory does not exist at {dirpath}")
    return path


def validate_fail_filepath(fail_filepath: Path) -> None:
    """Validates the tester file path format and existence."""
    if not str(fail_filepath).endswith(".fail"):
        print_and_exit(f"Error: fail_filepath should end in .fail: {fail_filepath}")
