"""Module to access and modify tag configuration files used by resmoke."""

import collections
import copy
from functools import cmp_to_key
import textwrap
import yaml


# Setup to preserve order in yaml.dump, see https://stackoverflow.com/a/8661021
def _represent_dict_order(self, data):
    return self.represent_mapping("tag:yaml.org,2002:map", list(data.items()))


yaml.add_representer(collections.OrderedDict, _represent_dict_order)

# End setup


class TagsConfig(object):
    """Represent a test tag configuration file."""

    def __init__(self, raw, cmp_func=None):
        """Initialize a TagsConfig from a dict representing the associations between tests and tags.

        'cmp_func' can be used to specify a comparison function that will be used when sorting tags.
        """

        self.raw = raw
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

    @classmethod
    def from_dict(cls, raw, **kwargs):
        """Return a TagsConfig from a dict representing the associations between tests and tags.

        See TagsConfig.__init__() for the keyword arguments that can be specified.
        """

        return cls(copy.deepcopy(raw), **kwargs)

    def get_test_kinds(self):
        """List the test kinds."""
        return list(self._conf.keys())

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

    def remove_tag(self, test_kind, test_pattern, tag):
        """Remove a tag. Return True if the tag was removed or False if the tag was not present."""
        patterns = self._conf.get(test_kind)
        if not patterns or test_pattern not in patterns:
            return False
        tags = patterns.get(test_pattern)
        if tags and tag in tags:
            tags[:] = (value for value in tags if value != tag)
            # Remove the pattern if there are no associated tags.
            if not tags:
                del patterns[test_pattern]
            return True
        return False

    def remove_test_pattern(self, test_kind, test_pattern):
        """Remove a test pattern."""
        patterns = self._conf.get(test_kind)
        if not patterns or test_pattern not in patterns:
            return
        del patterns[test_pattern]

    def is_modified(self):
        """Return True if the tags have been modified, False otherwise."""
        return self._conf != self._conf_copy

    def write_file(self, filename, preamble=None):
        """Write the tags to a file.

        If 'preamble' is present it will be added as a comment at the top of the file.
        """
        with open(filename, "w") as fstream:
            if preamble:
                print(
                    textwrap.fill(preamble, width=100, initial_indent="# ", subsequent_indent="# "),
                    file=fstream)

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
