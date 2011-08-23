# Copyright (c) 2009-2010 testtools developers. See LICENSE for details.

"""ContentType - a MIME Content Type."""


class ContentType(object):
    """A content type from http://www.iana.org/assignments/media-types/

    :ivar type: The primary type, e.g. "text" or "application"
    :ivar subtype: The subtype, e.g. "plain" or "octet-stream"
    :ivar parameters: A dict of additional parameters specific to the
        content type.
    """

    def __init__(self, primary_type, sub_type, parameters=None):
        """Create a ContentType."""
        if None in (primary_type, sub_type):
            raise ValueError("None not permitted in %r, %r" % (
                primary_type, sub_type))
        self.type = primary_type
        self.subtype = sub_type
        self.parameters = parameters or {}

    def __eq__(self, other):
        if type(other) != ContentType:
            return False
        return self.__dict__ == other.__dict__

    def __repr__(self):
        return "%s/%s params=%s" % (self.type, self.subtype, self.parameters)


UTF8_TEXT = ContentType('text', 'plain', {'charset': 'utf8'})
