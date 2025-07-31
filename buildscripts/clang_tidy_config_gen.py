import argparse
import os

parser = argparse.ArgumentParser()
parser.add_argument("--input", required=True, type=str)
parser.add_argument("--warnings-as-errors", action="store_true")
parser.add_argument("--output", required=True, type=str)
args = parser.parse_args()

rule_dir = os.path.dirname(args.output).replace(os.getcwd(), "")

with open(args.input) as f:
    content = f.read()

content = content.replace("@MONGO_BUILD_DIR@", f".*/{rule_dir}/src/mongo/.*/.*_gen.h")
content = content.replace("@MONGO_BRACKET_BUILD_DIR@", f"{rule_dir}/src/mongo")
if args.warnings_as_errors:
    content += 'WarningsAsErrors: "*"\n'

with open(args.output, "w") as f:
    f.write(content)
