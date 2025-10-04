#!/bin/env python

import os
import sys


def get_samples(input_files):
    for file_name in input_files:
        if not os.path.isfile(file_name):
            raise RuntimeError("Not a file: " + file_name)
        with open(file_name, 'r') as input_file:
            yield from input_file


def process_files(output_folder, input_files):
    if not os.path.exists(output_folder):
        os.makedirs(output_folder)

    for i, sample in enumerate(get_samples(input_files)):
        with open(os.path.join(output_folder, str(i) + ".txt"), 'w') as output_file:
            output_file.write(sample)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python script.py <output_folder> <input_file1> [<input_file2> ...]")
        sys.exit(1)

    process_files(output_folder=sys.argv[1], input_files=sys.argv[2:])
