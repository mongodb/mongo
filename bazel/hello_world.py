# TODO SERVER-80633 remove this when we have rules which use the toolchain
import networkx
import argparse

parser = argparse.ArgumentParser()
parser.add_argument("--output-file")
parser.add_argument("--hello-target")
args = parser.parse_args()

g = networkx.Graph()

important_message = f"Hello {args.hello_target}!, I made a {type(g)}"

print(important_message)

with open(args.output_file, 'w') as f:
    f.write(important_message)
