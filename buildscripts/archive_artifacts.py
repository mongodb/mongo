import argparse
import fnmatch
import glob
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile


def create_tarball(output_filename, file_patterns, exclude_patterns):
    if exclude_patterns is None:
        exclude_patterns = []

    included_files = set()

    for pattern in file_patterns:
        try:
            found_files = glob.glob(pattern, recursive=True)
            if not found_files:
                print(f"Warning: No files found for pattern '{pattern}'", file=sys.stderr)
            else:
                for f in found_files:
                    if os.path.isfile(f) or os.path.islink(f):
                         included_files.add(f)
        except Exception as e:
            print(f"Error processing pattern '{pattern}': {e}", file=sys.stderr)

    files_to_add = set()
    if exclude_patterns:
        for file_path in included_files:
            is_excluded = False
            for pattern in exclude_patterns:
                if fnmatch.fnmatch(file_path, pattern):
                    is_excluded = True
                    break
            if not is_excluded:
                files_to_add.add(file_path)
    else:
        files_to_add = included_files

    print(f"Creating tarball: {output_filename}")
    try:
        if shutil.which("pigz"):
            with tempfile.NamedTemporaryFile(mode="w+", encoding="utf-8") as tmp_file:
                for file in sorted(list(files_to_add)):
                    tmp_file.write(file + '\n')
                tmp_file.flush()
                tar_command = ["tar", "--dereference", "--use-compress-program", "pigz", "-cf", output_filename, "-T", tmp_file.name]
                subprocess.run(
                    tar_command,
                    check=True,
                    text=True
                )
        else:
            print("pigz not found. Using serial compression")
            with tarfile.open(output_filename, "w:gz", dereference=True) as tar:
                for file_path in sorted(list(files_to_add)):
                    tar.add(file_path, file_path)
        print("Tarball created successfully.")

    except Exception as e:
        print(f"Error creating tarball: {e}", file=sys.stderr)
        raise e


if __name__ == "__main__":
    os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))

    parser = argparse.ArgumentParser(
        description="Create a gzipped tarball from file patterns, dereferencing symlinks.",
        formatter_class=argparse.RawTextHelpFormatter,
    )

    parser.add_argument(
        "-o", "--output",
        required=True,
        help="The name of the output tarball file (e.g., archive.tar.gz)."
    )

    parser.add_argument(
        "--base_dir",
        default=".",
        help="Directory to run in."
    )
    
    parser.add_argument(
        "-e", "--exclude",
        action='append',
        default=[],
        help="A file pattern to exclude (e.g., '**/__pycache__/*'). Can be specified multiple times."
    )

    parser.add_argument(
        "patterns",
        nargs='+',
        help="One or more file patterns to include. Use quotes around patterns with wildcards."
    )

    args = parser.parse_args()

    os.chdir(args.base_dir)

    create_tarball(args.output, args.patterns, args.exclude)
