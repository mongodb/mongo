# Copyright (C) 2018 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html

import io

class CommentStripper(object):
    """Removes lines starting with "//" from a file stream."""

    def __init__(self, f):
        self.f = f
        self.state = 0

    def read(self, size=-1):
        bytes = self.f.read(size)
        # TODO: Do we need to read more bytes if comments were stripped
        # in order to obey the size request?
        return "".join(self._strip_comments(bytes))

    def _strip_comments(self, bytes):
        for byte in bytes:
            if self.state == 0:
                # state 0: start of a line
                if byte == "/":
                    self.state = 1
                elif byte == "\n":
                    self.state = 0
                    yield byte
                else:
                    self.state = 2
                    yield byte
            elif self.state == 1:
                # state 1: read a single '/'
                if byte == "/":
                    self.state = 3
                elif byte == "\n":
                    self.state = 0
                    yield "/"  # the one that was skipped
                    yield "\n"
                else:
                    self.state = 2
                    yield "/"  # the one that was skipped
                    yield byte
            elif self.state == 2:
                # state 2: middle of a line, no comment
                if byte == "\n":
                    self.state = 0
                yield byte
            elif self.state == 3:
                # state 3: inside a comment
                if byte == "\n":
                    self.state = 0
