#!/usr/bin/env python3
"""Stub file pointing users to the new invocation."""

if __name__ == "__main__":
    print(
        "Hello! It seems you've executed 'buildscripts/setup_multiversion_mongodb.py'. We have\n"
        "moved the functionality into a standalone tool called `db-contrib-tool`. You can\n"
        "install it with either `pip install db-contrib-tool`, or `pipx install db-contrib-tool`.\n"
        "\n"
        "The latter ensures the tool is in the global PATH. See installation instructions for `pipx`\n"
        "here if you don't have it:\n"
        "https://github.com/pypa/pipx#on-linux-install-via-pip-requires-pip-190-or-later"
    )
