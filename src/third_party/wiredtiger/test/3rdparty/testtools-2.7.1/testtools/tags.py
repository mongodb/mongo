# Copyright (c) 2012 testtools developers. See LICENSE for details.

"""Tag support."""


class TagContext:
    """A tag context."""

    def __init__(self, parent=None):
        """Create a new TagContext.

        :param parent: If provided, uses this as the parent context.  Any tags
            that are current on the parent at the time of construction are
            current in this context.
        """
        self.parent = parent
        self._tags = set()
        if parent:
            self._tags.update(parent.get_current_tags())

    def get_current_tags(self):
        """Return any current tags."""
        return set(self._tags)

    def change_tags(self, new_tags, gone_tags):
        """Change the tags on this context.

        :param new_tags: A set of tags to add to this context.
        :param gone_tags: A set of tags to remove from this context.
        :return: The tags now current on this context.
        """
        self._tags.update(new_tags)
        self._tags.difference_update(gone_tags)
        return self.get_current_tags()
