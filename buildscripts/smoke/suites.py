"""
Utilities for searching a database of tests based on a query over tags provided by the tests.
The resulting search becomes a test suite.
"""

import re


class RegexQuery(object):

    """A query based on regex includes/excludes.

    TODO: Something more complicated, or link to actual MongoDB queries?

    """

    def __init__(self,
                 include_res=[],
                 include_except_res=[],
                 exclude_res=[],
                 exclude_except_res=[]):

        self.include_res = []
        self.include_res.extend([(include_re, False) for include_re in include_res])
        self.include_res.extend([(include_except_re, True)
                                 for include_except_re in include_except_res])

        self.exclude_res = []
        self.exclude_res.extend([(exclude_re, False) for exclude_re in exclude_res])
        self.exclude_res.extend([(exclude_except_re, True)
                                 for exclude_except_re in exclude_except_res])

    def matches(self, value):
        return self.matches_values([value])

    def matches_values(self, values):

        # First see if anything in the values make us included
        included = True

        if self.include_res:

            for include_re, invert_match in self.include_res:

                if not invert_match:

                    # Include if any of the values is matched by an include pattern
                    included = False
                    for value in values:
                        if include_re.search(value):
                            included = True
                            break
                else:

                    # Include if all of the values are not matched by an include except pattern
                    included = True
                    for value in values:
                        if include_re.search(value):
                            included = False
                            break

                if included == True:
                    break

        if not included:
            return included

        if self.exclude_res:

            for exclude_re, invert_match in self.exclude_res:

                if not invert_match:

                    # Exclude if any of the values are matched by an exclude pattern
                    included = True
                    for value in values:
                        if exclude_re.search(value):
                            included = False
                            break
                else:

                    # Exclude if all of the values are not matched by an exclude except patt
                    included = False
                    for value in values:
                        if exclude_re.search(value):
                            included = True
                            break

                if included == False:
                    break

        return included

    def combine(self, other):
        self.include_res.extend(other.include_res)
        self.exclude_res.extend(other.exclude_res)


def build_suite(tests, tag_query):

    # Filter tests by tag
    def tags_match(test):
        return tag_query.matches_values(test.tags)

    return filter(tags_match, tests)
