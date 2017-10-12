#!/usr/bin/env python

#    Copyright 2012 10gen Inc.
#
#    This program is free software: you can redistribute it and/or  modify
#    it under the terms of the GNU Affero General Public License, version 3,
#    as published by the Free Software Foundation.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU Affero General Public License for more details.
#
#    You should have received a copy of the GNU Affero General Public License
#    along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
#    As a special exception, the copyright holders give permission to link the
#    code of portions of this program with the OpenSSL library under certain
#    conditions as described in each individual source file and distribute
#    linked combinations including the program with the OpenSSL library. You
#    must comply with the GNU Affero General Public License in all respects
#    for all of the code used other than as permitted herein. If you modify
#    file(s) with this exception, you may extend this exception to your
#    version of the file(s), but you are not obligated to do so. If you do not
#    wish to do so, delete this exception statement from your version. If you
#    delete this exception statement from all source files in the program,
#    then also delete it in the license file.

"""Generate error_codes.{h,cpp} from error_codes.err.

Format of error_codes.err:

error_code("symbol1", code1)
error_code("symbol2", code2)
...
error_class("class1", ["symbol1", "symbol2, ..."])

Usage:
    python generate_error_codes.py <path to error_codes.err> <template>=<output>...
"""

usage_msg = "usage: %prog /path/to/error_codes.err <template>=<output>..."

from collections import namedtuple
from Cheetah.Template import Template
import sys

def render_template(template_path, **kw):
    '''Renders the template file located at template_path, using the variables defined by kw, and
       returns the result as a string'''

    template = Template.compile(
            file=template_path,
            compilerSettings=dict(directiveStartToken="//#",directiveEndToken="//#"),
            baseclass=dict,
            useCache=False)
    return str(template(**kw))

class ErrorCode:
    def __init__(self, name, code):
        self.name = name
        self.code = code
        self.categories = []

class ErrorClass:
    def __init__(self, name, codes):
        self.name = name
        self.codes = codes

def main(argv):
    # Parse and validate argv.
    if len(sys.argv) < 2:
        usage("Must specify error_codes.err")
    if len(sys.argv) < 3:
        usage("Must specify at least one template=output pair")

    template_outputs = []
    for arg in sys.argv[2:]:
        try:
            template, output = arg.split('=', 1)
            template_outputs.append((template, output))
        except Exception:
            usage("Error parsing template=output pair: " + arg)

    # Parse and validate error_codes.err.
    error_codes, error_classes = parse_error_definitions_from_file(argv[1])
    check_for_conflicts(error_codes, error_classes)

    # Render the templates to the output files.
    for template, output in template_outputs:
        text = render_template(template,
                codes=error_codes,
                categories=error_classes,
                )

        with open(output, 'wb') as outfile:
            outfile.write(text)

def die(message=None):
    sys.stderr.write(message or "Fatal error\n")
    sys.exit(1)

def usage(message=None):
    sys.stderr.write(__doc__)
    die(message)

def parse_error_definitions_from_file(errors_filename):
    errors_file = open(errors_filename, 'r')
    errors_code = compile(errors_file.read(), errors_filename, 'exec')
    error_codes = []
    error_classes = []
    eval(errors_code,
            dict(error_code=lambda *args, **kw: error_codes.append(ErrorCode(*args, **kw)),
                 error_class=lambda *args: error_classes.append(ErrorClass(*args))))
    error_codes.sort(key=lambda x: x.code)

    return error_codes, error_classes

def check_for_conflicts(error_codes, error_classes):
    failed = has_duplicate_error_codes(error_codes)
    if has_duplicate_error_classes(error_classes):
        failed = True
    if has_missing_error_codes(error_codes, error_classes):
        failed = True
    if failed:
        die()

def has_duplicate_error_codes(error_codes):
    sorted_by_name = sorted(error_codes, key=lambda x: x.name)
    sorted_by_code = sorted(error_codes, key=lambda x: x.code)

    failed = False
    prev = sorted_by_name[0]
    for curr in sorted_by_name[1:]:
        if curr.name == prev.name:
            sys.stdout.write('Duplicate name %s with codes %s and %s\n'
                    % (curr.name, curr.code, prev.code))
            failed = True
        prev = curr

    prev = sorted_by_code[0]
    for curr in sorted_by_code[1:]:
        if curr.code == prev.code:
            sys.stdout.write('Duplicate code %s with names %s and %s\n'
                    % (curr.code, curr.name, prev.name))
            failed = True
        prev = curr

    return failed

def has_duplicate_error_classes(error_classes):
    names = sorted(ec.name for ec in error_classes)

    failed = False
    prev_name = names[0]
    for name in names[1:]:
        if prev_name == name:
            sys.stdout.write('Duplicate error class name %s\n' % name)
            failed = True
        prev_name = name
    return failed

def has_missing_error_codes(error_codes, error_classes):
    code_names = dict((ec.name, ec) for ec in error_codes)
    failed = False
    for category in error_classes:
        for name in category.codes:
            try:
                code_names[name].categories.append(category.name)
            except KeyError:
                sys.stdout.write('Undeclared error code %s in class %s\n' % (name, category.name))
                failed = True

    return failed

if __name__ == '__main__':
    main(sys.argv)
