# Copyright (C) 2018 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html

from collections import namedtuple

LocalFile = namedtuple("LocalFile", ["dirname", "filename"])
SrcFile = namedtuple("SrcFile", ["filename"])
InFile = namedtuple("InFile", ["filename"])
TmpFile = namedtuple("TmpFile", ["filename"])
OutFile = namedtuple("OutFile", ["filename"])
PkgFile = namedtuple("PkgFile", ["filename"])

IcuTool = namedtuple("IcuTool", ["name"])
SystemTool = namedtuple("SystemTool", ["name"])

DepTarget = namedtuple("DepTarget", ["name"])
