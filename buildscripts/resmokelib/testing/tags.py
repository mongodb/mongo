"""Module to access and modify tag configuration files used by resmoke."""

import collections
import copy
import textwrap
from functools import cmp_to_key

import yaml

from buildscripts.resmokelib.utils import default_if_none


# Setup to preserve order in yaml.dump, see https://stackoverflow.com/a/8661021
def _represent_dict_order(self, data):
    return self.represent_mapping("tag:yaml.org,2002:map", list(data.items()))


yaml.add_representer(collections.OrderedDict, _represent_dict_order)

# End setup


class TagsConfig(object):
    """Represent a test tag configuration file."""

    def __init__(self, raw=None, cmp_func=None):
        """Initialize a TagsConfig from a dict representing the associations between tests and tags.

        'raw' is a dict containing a 'selector' key, whose value is a dict mapping test kinds to tests to tags.
        'cmp_func' can be used to specify a comparison function that will be used when sorting tags.
        """

        self.raw = default_if_none(raw, {"selector": {}})
        self._conf = self.raw["selector"]
        self._conf_copy = copy.deepcopy(self._conf)
        self._cmp_func = cmp_func

    @classmethod
    def from_file(cls, filename, **kwargs):
        """Return a TagsConfig from a file containing the associations between tests and tags.

        See TagsConfig.__init__() for the keyword arguments that can be specified.
        """

        with open(filename, "r") as fstream:
            raw = yaml.safe_load(fstream)

        return cls(raw, **kwargs)

    def get_test_patterns(self, test_kind):
        """List the test patterns under 'test_kind'."""
        return list(getdefault(self._conf, test_kind, {}).keys())

    def get_tags(self, test_kind, test_pattern):
        """List the tags under 'test_kind' and 'test_pattern'."""
        patterns = getdefault(self._conf, test_kind, {})
        return getdefault(patterns, test_pattern, [])

    def add_tag(self, test_kind, test_pattern, tag):
        """Add a tag. Return True if the tag is added or False if the tag was already present."""
        patterns = setdefault(self._conf, test_kind, {})
        tags = setdefault(patterns, test_pattern, [])
        if tag not in tags:
            tags.append(tag)
            tags.sort(key=cmp_to_key(self._cmp_func) if self._cmp_func else None)
            return True
        return False

    def write_file(self, filename, preamble=None):
        """Write the tags to a file.

        If 'preamble' is present it will be added as a comment at the top of the file.
        """
        with open(filename, "w") as fstream:
            if preamble:
                print(
                    textwrap.fill(preamble, width=100, initial_indent="# ", subsequent_indent="# "),
                    file=fstream,
                )

            # We use yaml.safe_dump() in order avoid having strings being written to the file as
            # "!!python/unicode ..." and instead have them written as plain 'str' instances.
            yaml.safe_dump(self.raw, fstream, default_flow_style=False)


def getdefault(doc, key, default):
    """Return the value in 'doc' with key 'key' if present and not None.

    Return the specified default value otherwise.
    """
    value = doc.get(key)
    if value is not None:
        return value
    return default


def setdefault(doc, key, default):
    """Return the value in 'doc' with key 'key' if present and not None.

    Otherwise set the value to default and return it.
    """
    value = doc.setdefault(key, default)
    if value is not None:
        return value
    doc[key] = default
    return default
