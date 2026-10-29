#!/usr/bin/env python3
import argparse

FILE_HEADER = """// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// Generated file. Do not edit.
#pragma once
#include <string_view>

namespace mongo {
namespace extension{
namespace host {\n"""


FILE_FOOTER = """} // namespace host
} // namespace extension
} // namespace mongo
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--public_key_path", dest="public_key_path", required=True)
    ap.add_argument("--embedded_key_header_path", required=True)
    args = ap.parse_args()

    with open(args.public_key_path, "r") as f:
        public_key_contents = f.read()

    # Write header
    with open(args.embedded_key_header_path, "w") as h:
        h.write(FILE_HEADER)
        public_key_definition = """static constexpr std::string_view kMongoExtensionSigningPublicKey = R\"({0})\";\n""".format(
            public_key_contents
        )
        h.write(public_key_definition)
        h.write(FILE_FOOTER)


if __name__ == "__main__":
    main()
