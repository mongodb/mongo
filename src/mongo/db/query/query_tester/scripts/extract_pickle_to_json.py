"""
Script to automate the conversion of a pickle file (.pkl), containing multiple
aggregated DataFrames across test files, into a JSON file. This JSON file is intended
to be sent to QueryTester for further feature processing, analysis, and display.

The script reads the pickle file, converts the DataFrames into a JSON format, and saves
the result as a JSON file. The resulting file is then ready for use in subsequent
processing steps within the QueryTester framework.
"""

import subprocess
from pathlib import Path

import utils

# Parse and validate arguments
args = utils.parse_args_common(
    "Convert pickle file to JSON and move to QueryTester for further processing."
)
output_prefix = args.output_prefix

# Validate directories and change to feature-extractor directory
mongo_repo_root = utils.validate_directory(args.mongo_repo_root)
feature_extractor_dir = utils.validate_and_change_directory(args.feature_extractor_dir)

# Convert pickle file with aggregated dataframes to JSON
pickle_to_json_command = ["bin/venv", "pickle_to_json.py", output_prefix]
subprocess.run(pickle_to_json_command, check=True)

# Clean up pickle file and move JSON to QueryTester
pkl_file, json_file = utils.construct_filenames(output_prefix)

# Remove the pickle file
utils.clean_up_file(pkl_file, "Pickle file")

# Move JSON file
querytester_path = Path(mongo_repo_root) / "src/mongo/db/query/query_tester"
utils.move_file(json_file, querytester_path / Path(json_file).name)
print("Process completed successfully.")
