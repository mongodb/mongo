# Copyright (c) 2012 testtools developers. See LICENSE for details.

"""Test tag support."""


from testtools import TestCase
from testtools.tags import TagContext


class TestTags(TestCase):

    def test_no_tags(self):
        # A tag context has no tags initially.
        tag_context = TagContext()
        self.assertEqual(set(), tag_context.get_current_tags())

    def test_add_tag(self):
        # A tag added with change_tags appears in get_current_tags.
        tag_context = TagContext()
        tag_context.change_tags(set(['foo']), set())
        self.assertEqual(set(['foo']), tag_context.get_current_tags())

    def test_add_tag_twice(self):
        # Calling change_tags twice to add tags adds both tags to the current
        # tags.
        tag_context = TagContext()
        tag_context.change_tags(set(['foo']), set())
        tag_context.change_tags(set(['bar']), set())
        self.assertEqual(
            set(['foo', 'bar']), tag_context.get_current_tags())

    def test_change_tags_returns_tags(self):
        # change_tags returns the current tags.  This is a convenience.
        tag_context = TagContext()
        tags = tag_context.change_tags(set(['foo']), set())
        self.assertEqual(set(['foo']), tags)

    def test_remove_tag(self):
        # change_tags can remove tags from the context.
        tag_context = TagContext()
        tag_context.change_tags(set(['foo']), set())
        tag_context.change_tags(set(), set(['foo']))
        self.assertEqual(set(), tag_context.get_current_tags())

    def test_child_context(self):
        # A TagContext can have a parent.  If so, its tags are the tags of the
        # parent at the moment of construction.
        parent = TagContext()
        parent.change_tags(set(['foo']), set())
        child = TagContext(parent)
        self.assertEqual(
            parent.get_current_tags(), child.get_current_tags())

    def test_add_to_child(self):
        # Adding a tag to the child context doesn't affect the parent.
        parent = TagContext()
        parent.change_tags(set(['foo']), set())
        child = TagContext(parent)
        child.change_tags(set(['bar']), set())
        self.assertEqual(set(['foo', 'bar']), child.get_current_tags())
        self.assertEqual(set(['foo']), parent.get_current_tags())

    def test_remove_in_child(self):
        # A tag that was in the parent context can be removed from the child
        # context without affect the parent.
        parent = TagContext()
        parent.change_tags(set(['foo']), set())
        child = TagContext(parent)
        child.change_tags(set(), set(['foo']))
        self.assertEqual(set(), child.get_current_tags())
        self.assertEqual(set(['foo']), parent.get_current_tags())

    def test_parent(self):
        # The parent can be retrieved from a child context.
        parent = TagContext()
        parent.change_tags(set(['foo']), set())
        child = TagContext(parent)
        child.change_tags(set(), set(['foo']))
        self.assertEqual(parent, child.parent)


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
