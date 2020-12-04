#!/usr/bin/env python3
"""Stub file pointing users to the new invocation."""

if __name__ == "__main__":
    print(
        "Hello! It seems you've executed 'buildscripts/setup_multiversion_mongodb.py'. We have\n"
        "repackaged the setup multiversion as a subcommand of resmoke. It can now be invoked as\n"
        "'./buildscripts/resmoke.py setup-multiversion' with all of the same arguments as before.\n"
        "Please use './buildscripts/resmoke.py setup-multiversion --help' for more details.")
