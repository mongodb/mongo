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

"""Python nodes."""

import SCons.Node

_memo_lookup_map = {}


class ValueNodeInfo(SCons.Node.NodeInfoBase):
    __slots__ = ('csig',)
    current_version_id = 2

    field_list = ['csig']

    def str_to_node(self, s):
        return ValueWithMemo(s)


class ValueBuildInfo(SCons.Node.BuildInfoBase):
    __slots__ = ()
    current_version_id = 2


class Value(SCons.Node.Node):
    """A Node class for values represented by Python expressions.

    Values are typically passed on the command line or generated
    by a script, but not from a file or some other source.

    .. versionchanged:: 4.0
       the *name* parameter was added.
    """

    NodeInfo = ValueNodeInfo
    BuildInfo = ValueBuildInfo

    def __init__(self, value, built_value=None, name=None) -> None:
        super().__init__()
        self.value = value
        self.changed_since_last_build = 6
        self.store_info = 0
        if built_value is not None:
            self.built_value = built_value

        # Set a name so it can be a child of a node and not break
        # its parent's implementation of Node.get_contents.
        if name:
            self.name = name
        else:
            self.name = str(value)

    def str_for_display(self):
        return repr(self.value)

    def __str__(self) -> str:
        return str(self.value)

    def make_ready(self) -> None:
        self.get_csig()

    def build(self, **kw) -> None:
        if not hasattr(self, 'built_value'):
            SCons.Node.Node.build(self, **kw)

    is_up_to_date = SCons.Node.Node.children_are_up_to_date

    def is_under(self, dir) -> bool:
        # Make Value nodes get built regardless of
        # what directory scons was run from. Value nodes
        # are outside the filesystem:
        return True

    def write(self, built_value) -> None:
        """Set the value of the node."""
        self.built_value = built_value

    def read(self):
        """Return the value. If necessary, the value is built."""
        self.build()
        if not hasattr(self, 'built_value'):
            self.built_value = self.value
        return self.built_value

    def get_text_contents(self) -> str:
        """By the assumption that the node.built_value is a
        deterministic product of the sources, the contents of a Value
        are the concatenation of all the contents of its sources.  As
        the value need not be built when get_contents() is called, we
        cannot use the actual node.built_value."""
        ###TODO: something reasonable about universal newlines
        contents = str(self.value)
        for kid in self.children(None):
            # Get csig() value of child as this is more efficent
            contents = contents + kid.get_csig()
        return contents

    def get_contents(self) -> bytes:
        """Get contents for signature calculations."""
        return self.get_text_contents().encode()

    def get_csig(self, calc=None):
        """Because we're a Python value node and don't have a real
        timestamp, we get to ignore the calculator and just use the
        value contents.

        Returns string. Ideally string of hex digits. (Not bytes)
        """
        try:
            return self.ninfo.csig
        except AttributeError:
            pass

        contents = self.get_text_contents()

        self.get_ninfo().csig = contents
        return contents


def ValueWithMemo(value, built_value=None, name=None):
    """Memoized :class:`Value` node factory.

    .. versionchanged:: 4.0
       the *name* parameter was added.
    """
    global _memo_lookup_map

    # No current support for memoizing a value that needs to be built.
    if built_value:
        return Value(value, built_value, name=name)

    try:
        memo_lookup_key = hash((value, name))
    except TypeError:
        # Non-primitive types will hit this codepath.
        return Value(value, name=name)

    try:
        return _memo_lookup_map[memo_lookup_key]
    except KeyError:
        v = Value(value, built_value, name)
        _memo_lookup_map[memo_lookup_key] = v
        return v


# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
