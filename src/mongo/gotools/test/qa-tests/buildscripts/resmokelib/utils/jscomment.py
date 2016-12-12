"""
Utility for parsing JS comments.
"""

from __future__ import absolute_import

import re

import yaml


# TODO: use a more robust regular expression for matching tags
_JSTEST_TAGS_RE = re.compile(r".*@tags\s*:\s*(\[[^\]]*\])", re.DOTALL)


def get_tags(pathname):
    """
    Returns the list of tags found in the (JS-style) comments of
    'pathname'. The definition can span multiple lines, use unquoted,
    single-quoted, or double-quoted strings, and use the '#' character
    for inline commenting.

    e.g.

     /**
      * @tags: [ "tag1",  # double quoted
      *          'tag2'   # single quoted
      *                   # line with only a comment
      *         , tag3    # no quotes
      *           tag4,   # trailing comma
      *         ]
      */
    """

    with open(pathname) as fp:
        match = _JSTEST_TAGS_RE.match(fp.read())
        if match:
            try:
                # TODO: it might be worth supporting the block (indented) style of YAML lists in
                #       addition to the flow (bracketed) style
                tags = yaml.safe_load(_strip_jscomments(match.group(1)))
                if not isinstance(tags, list) and all(isinstance(tag, basestring) for tag in tags):
                    raise TypeError("Expected a list of string tags, but got '%s'" % (tags))
                return tags
            except yaml.YAMLError as err:
                raise ValueError("File '%s' contained invalid tags (expected YAML): %s"
                                 % (pathname, err))

    return []


def _strip_jscomments(s):
    """
    Given a string 's' that represents the contents after the "@tags:"
    annotation in the JS file, this function returns a string that can
    be converted to YAML.

    e.g.

        [ "tag1",  # double quoted
      *   'tag2'   # single quoted
      *            # line with only a comment
      *  , tag3    # no quotes
      *    tag4,   # trailing comma
      * ]

    If the //-style JS comments were used, then the example remains the,
    same except with the '*' character is replaced by '//'.
    """

    yaml_lines = []

    for line in s.splitlines():
        # Remove leading whitespace and symbols that commonly appear in JS comments.
        line = line.lstrip("\t ").lstrip("*/")
        yaml_lines.append(line)

    return "\n".join(yaml_lines)
