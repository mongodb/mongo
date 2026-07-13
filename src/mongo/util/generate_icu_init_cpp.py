#!/usr/bin/env python3
# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0

import optparse
import sys


def main(argv):
    parser = optparse.OptionParser()
    parser.add_option(
        "-o", "--output", action="store", dest="output_cpp_file", help="path to output cpp file"
    )
    parser.add_option(
        "-i",
        "--input",
        action="store",
        dest="input_data_file",
        help="input ICU data file, in common format (.dat)",
    )
    (options, args) = parser.parse_args(argv)
    if len(args) > 1:
        parser.error("too many arguments")
    if options.output_cpp_file is None:
        parser.error("output file unspecified")
    if options.input_data_file is None:
        parser.error("input ICU data file unspecified")
    generate_cpp_file(options.input_data_file, options.output_cpp_file)


def generate_cpp_file(data_file_path, cpp_file_path):
    source_template = """// AUTO-GENERATED FILE DO NOT EDIT
// See generate_icu_init_cpp.py.
// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include <unicode/udata.h>

#include "mongo/base/init.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

// alignas() is used here to ensure 16-alignment of ICU data.  See the following excerpt from the
// ICU user guide (<http://userguide.icu-project.org/icudata#TOC-Alignment>):
//
// "ICU data is designed to be 16-aligned, with natural alignment of values inside the data
// structure, so that the data is usable as is when memory-mapped.  Memory-mapping (as well as
// memory allocation) provides at least 16-alignment on modern platforms. Some CPUs require
// n-alignment of types of size n bytes (and crash on unaligned reads), other CPUs usually operate
// faster on data that is aligned properly.  Some of the ICU code explicitly checks for proper
// alignment."
alignas(16) const uint8_t kRawData[] = {%(decimal_encoded_data)s};

}  // namespace

MONGO_INITIALIZER_GENERAL(LoadICUData, (), ("BeginStartupOptionHandling"))(
        InitializerContext* context) {
    UErrorCode status = U_ZERO_ERROR;
    udata_setCommonData(kRawData, &status);
    fassert(40089, U_SUCCESS(status));
}

}  // namespace mongo
"""
    decimal_encoded_data = ""
    with open(data_file_path, "rb") as data_file:
        decimal_encoded_data = ",".join([str(byte) for byte in data_file.read()])
    with open(cpp_file_path, "w") as cpp_file:
        cpp_file.write(source_template % dict(decimal_encoded_data=decimal_encoded_data))


if __name__ == "__main__":
    main(sys.argv)
