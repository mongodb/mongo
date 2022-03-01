# Copyright (C) 2018 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html

from collections import namedtuple

MakeRule = namedtuple("MakeRule", ["name", "dep_literals", "dep_files", "output_file", "cmds"])

MakeFilesVar = namedtuple("MakeFilesVar", ["name", "files"])

MakeStringVar = namedtuple("MakeStringVar", ["name", "content"])
