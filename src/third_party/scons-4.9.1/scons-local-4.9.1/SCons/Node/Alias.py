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

"""Alias nodes.

This creates a hash of global Aliases (dummy targets).
"""

import collections

import SCons.Errors
import SCons.Node
import SCons.Util
from SCons.Util import hash_signature

class AliasNameSpace(collections.UserDict):
    def Alias(self, name, **kw):
        if isinstance(name, SCons.Node.Alias.Alias):
            return name
        try:
            a = self[name]
        except KeyError:
            a = SCons.Node.Alias.Alias(name, **kw)
            self[name] = a
        return a

    def lookup(self, name, **kw):
        try:
            return self[name]
        except KeyError:
            return None

class AliasNodeInfo(SCons.Node.NodeInfoBase):
    __slots__ = ('csig',)
    current_version_id = 2
    field_list = ['csig']
    def str_to_node(self, s):
        return default_ans.Alias(s)

class AliasBuildInfo(SCons.Node.BuildInfoBase):
    __slots__ = ()
    current_version_id = 2

class Alias(SCons.Node.Node):

    NodeInfo = AliasNodeInfo
    BuildInfo = AliasBuildInfo

    def __init__(self, name) -> None:
        super().__init__()
        self.name = name
        self.changed_since_last_build = 1
        self.store_info = 0

    def str_for_display(self):
        return '"' + self.__str__() + '"'

    def __str__(self) -> str:
        return self.name

    def make_ready(self) -> None:
        self.get_csig()

    really_build = SCons.Node.Node.build
    is_up_to_date = SCons.Node.Node.children_are_up_to_date

    def is_under(self, dir) -> bool:
        # Make Alias nodes get built regardless of
        # what directory scons was run from. Alias nodes
        # are outside the filesystem:
        return True

    def get_contents(self):
        """The contents of an alias is the concatenation
        of the content signatures of all its sources."""
        childsigs = [n.get_csig() for n in self.children()]
        return ''.join(childsigs)

    def sconsign(self) -> None:
        """An Alias is not recorded in .sconsign files"""
        pass

    #
    #
    #

    def build(self, **kw) -> None:
        """A "builder" for aliases."""
        if len(self.executor.post_actions) + len(self.executor.pre_actions) > 0:
            # Only actually call Node's build() if there are any
            # pre or post actions.
            # Alias nodes will get 1 action and Alias.build()
            # This fixes GH Issue #2281
            return self.really_build(**kw)


    def convert(self) -> None:
        try: del self.builder
        except AttributeError: pass
        self.reset_executor()
        self.build = self.really_build

    def get_csig(self):
        """
        Generate a node's content signature, the digested signature
        of its content.

        node - the node
        cache - alternate node to use for the signature cache
        returns - the content signature
        """
        try:
            return self.ninfo.csig
        except AttributeError:
            pass

        contents = self.get_contents()
        csig = hash_signature(contents)
        self.get_ninfo().csig = csig
        return csig

default_ans = AliasNameSpace()

SCons.Node.arg2nodes_lookups.append(default_ans.lookup)

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
