#!/usr/bin/env python3
#
# Copyright (C) 2018-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.

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
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


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
