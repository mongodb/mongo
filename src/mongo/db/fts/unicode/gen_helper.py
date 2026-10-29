def getCopyrightNotice():
    return """// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
//
// THIS IS A GENERATED FILE, DO NOT MODIFY.\n\n"""


def openNamespaces():
    return "namespace mongo {\nnamespace unicode {\n\n"


def closeNamespaces():
    return "\n} //  namespace unicode\n} //  namespace mongo\n"


def include(header):
    return '#include "' + header + '"\n'
