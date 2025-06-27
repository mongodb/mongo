# MIT License
#
# Copyright The SCons Foundation
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

"""
MS compilers: Supported Windows chip architectures.
"""


class ArchDefinition:
    """
    A class for defining architecture-specific settings and logic.
    """
    def __init__(self, arch, synonyms=[]) -> None:
        self.arch = arch
        self.synonyms = synonyms

SupportedArchitectureList = [
    ArchDefinition(
        'x86',
        ['i386', 'i486', 'i586', 'i686'],
    ),
    ArchDefinition(
        'x86_64',
        ['AMD64', 'amd64', 'em64t', 'EM64T', 'x86_64'],
    ),
    ArchDefinition(
        'ia64',
        ['IA64'],
    ),
    ArchDefinition(
        'arm',
        ['ARM'],
    ),
]

SupportedArchitectureMap = {}
for a in SupportedArchitectureList:
    SupportedArchitectureMap[a.arch] = a
    for s in a.synonyms:
        SupportedArchitectureMap[s] = a

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
