#!/usr/bin/env python
"""Utility to return YAML value from key in YAML file."""

from __future__ import print_function

import optparse

import yaml


def get_yaml_value(yaml_file, yaml_key):
    """Return string value for 'yaml_key' from 'yaml_file.'"""
    with open(yaml_file, "r") as ystream:
        yaml_dict = yaml.safe_load(ystream)
    return str(yaml_dict.get(yaml_key, ""))


def main():
    """Execute Main program."""

    parser = optparse.OptionParser(description=__doc__)
    parser.add_option("--yamlFile", dest="yaml_file", default=None, help="YAML file to read")
    parser.add_option(
        "--yamlKey", dest="yaml_key", default=None, help="Top level YAML key to provide the value")

    (options, _) = parser.parse_args()
    if not options.yaml_file:
        parser.error("Must specifiy '--yamlFile'")
    if not options.yaml_key:
        parser.error("Must specifiy '--yamlKey'")

    print(get_yaml_value(options.yaml_file, options.yaml_key))

if __name__ == "__main__":
    main()
