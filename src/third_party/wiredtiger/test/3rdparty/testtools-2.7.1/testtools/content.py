# Copyright (c) 2009-2012 testtools developers. See LICENSE for details.

"""Content - a MIME-like Content object."""

__all__ = [
    'attach_file',
    'Content',
    'content_from_file',
    'content_from_stream',
    'json_content',
    'text_content',
    'TracebackContent',
    ]

import codecs
import functools
import json
import os
import traceback

from testtools.compat import _b
from testtools.content_type import ContentType, JSON, UTF8_TEXT


_join_b = _b("").join


DEFAULT_CHUNK_SIZE = 4096

STDOUT_LINE = '\nStdout:\n%s'
STDERR_LINE = '\nStderr:\n%s'


def _iter_chunks(stream, chunk_size, seek_offset=None, seek_whence=0):
    """Read 'stream' in chunks of 'chunk_size'.

    :param stream: A file-like object to read from.
    :param chunk_size: The size of each read from 'stream'.
    :param seek_offset: If non-None, seek before iterating.
    :param seek_whence: Pass through to the seek call, if seeking.
    """
    if seek_offset is not None:
        stream.seek(seek_offset, seek_whence)
    chunk = stream.read(chunk_size)
    while chunk:
        yield chunk
        chunk = stream.read(chunk_size)


class Content:
    """A MIME-like Content object.

    'Content' objects can be serialised to bytes using the iter_bytes method.
    If the 'Content-Type' is recognised by other code, they are welcome to
    look for richer contents that mere byte serialisation - for example in
    memory object graphs etc. However, such code MUST be prepared to receive
    a generic 'Content' object that has been reconstructed from a byte stream.

    :ivar content_type: The content type of this Content.
    """

    def __init__(self, content_type, get_bytes):
        """Create a ContentType."""
        if None in (content_type, get_bytes):
            raise ValueError("None not permitted in {!r}, {!r}".format(
                content_type, get_bytes))
        self.content_type = content_type
        self._get_bytes = get_bytes

    def __eq__(self, other):
        return (self.content_type == other.content_type and
            _join_b(self.iter_bytes()) == _join_b(other.iter_bytes()))

    def as_text(self):
        """Return all of the content as text.

        This is only valid where ``iter_text`` is.  It will load all of the
        content into memory.  Where this is a concern, use ``iter_text``
        instead.
        """
        return ''.join(self.iter_text())

    def iter_bytes(self):
        """Iterate over bytestrings of the serialised content."""
        return self._get_bytes()

    def iter_text(self):
        """Iterate over the text of the serialised content.

        This is only valid for text MIME types, and will use ISO-8859-1 if
        no charset parameter is present in the MIME type. (This is somewhat
        arbitrary, but consistent with RFC2617 3.7.1).

        :raises ValueError: If the content type is not "text/*".
        """
        if self.content_type.type != "text":
            raise ValueError("Not a text type %r" % self.content_type)
        return self._iter_text()

    def _iter_text(self):
        """Worker for iter_text - does the decoding."""
        encoding = self.content_type.parameters.get('charset', 'ISO-8859-1')
        decoder = codecs.getincrementaldecoder(encoding)()
        for bytes in self.iter_bytes():
            yield decoder.decode(bytes)
        final = decoder.decode(_b(''), True)
        if final:
            yield final

    def __repr__(self):
        return "<Content type={!r}, value={!r}>".format(
            self.content_type, _join_b(self.iter_bytes()))


class StackLinesContent(Content):
    """Content object for stack lines.

    This adapts a list of "preprocessed" stack lines into a 'Content' object.
    The stack lines are most likely produced from ``traceback.extract_stack``
    or ``traceback.extract_tb``.

    text/x-traceback;language=python is used for the mime type, in order to
    provide room for other languages to format their tracebacks differently.
    """

    # Whether or not to hide layers of the stack trace that are
    # unittest/testtools internal code.  Defaults to True since the
    # system-under-test is rarely unittest or testtools.
    HIDE_INTERNAL_STACK = True

    def __init__(self, stack_lines, prefix_content="", postfix_content=""):
        """Create a StackLinesContent for ``stack_lines``.

        :param stack_lines: A list of preprocessed stack lines, probably
            obtained by calling ``traceback.extract_stack`` or
            ``traceback.extract_tb``.
        :param prefix_content: If specified, a unicode string to prepend to the
            text content.
        :param postfix_content: If specified, a unicode string to append to the
            text content.
        """
        content_type = ContentType('text', 'x-traceback',
            {"language": "python", "charset": "utf8"})
        value = prefix_content + \
            self._stack_lines_to_unicode(stack_lines) + \
            postfix_content
        super().__init__(
            content_type, lambda: [value.encode("utf8")])

    def _stack_lines_to_unicode(self, stack_lines):
        """Converts a list of pre-processed stack lines into a unicode string.
        """
        msg_lines = traceback.format_list(stack_lines)
        return ''.join(msg_lines)


class TracebackContent(Content):
    """Content object for tracebacks.

    This adapts an exc_info tuple to the 'Content' interface.
    'text/x-traceback;language=python' is used for the mime type, in order to
    provide room for other languages to format their tracebacks differently.
    """

    def __init__(self, err, test, capture_locals=False):
        """Create a TracebackContent for ``err``.

        :param err: An exc_info error tuple.
        :param test: A test object used to obtain failureException.
        :param capture_locals: If true, show locals in the traceback.
        """
        if err is None:
            raise ValueError("err may not be None")

        exctype, value, tb = err
        # Skip test runner traceback levels
        if StackLinesContent.HIDE_INTERNAL_STACK:
            while tb and '__unittest' in tb.tb_frame.f_globals:
                tb = tb.tb_next

        limit = None
        # Disabled due to https://bugs.launchpad.net/testtools/+bug/1188420
        if (False
            and StackLinesContent.HIDE_INTERNAL_STACK
            and test.failureException
            and isinstance(value, test.failureException)):
            # Skip assert*() traceback levels
            limit = 0
            while tb and not self._is_relevant_tb_level(tb):
                limit += 1
                tb = tb.tb_next

        stack_lines = list(traceback.TracebackException(exctype, value, tb,
            limit=limit, capture_locals=capture_locals).format())
        content_type = ContentType('text', 'x-traceback',
            {"language": "python", "charset": "utf8"})
        super().__init__(
            content_type, lambda: [x.encode('utf8') for x in stack_lines])


def StacktraceContent(prefix_content="", postfix_content=""):
    """Content object for stack traces.

    This function will create and return a 'Content' object that contains a
    stack trace.

    The mime type is set to 'text/x-traceback;language=python', so other
    languages can format their stack traces differently.

    :param prefix_content: A unicode string to add before the stack lines.
    :param postfix_content: A unicode string to add after the stack lines.
    """
    stack = traceback.walk_stack(None)
    def filter_stack(stack):
        # Discard the filter_stack frame.
        next(stack)
        # Discard the StacktraceContent frame.
        next(stack)
        for f, f_lineno in stack:
            if StackLinesContent.HIDE_INTERNAL_STACK:
                if '__unittest' in f.f_globals:
                    return
                yield f, f_lineno
    extract = traceback.StackSummary.extract(filter_stack(stack))
    extract.reverse()
    return StackLinesContent(extract, prefix_content, postfix_content)


def json_content(json_data):
    """Create a JSON Content object from JSON-encodeable data."""
    data = json.dumps(json_data)
    # The json module perversely returns native str not bytes
    data = data.encode('utf8')
    return Content(JSON, lambda: [data])


def text_content(text):
    """Create a Content object from some text.

    This is useful for adding details which are short strings.
    """
    if not isinstance(text, str):
        raise TypeError(
            "text_content must be given text, not '%s'." % type(text).__name__
        )
    return Content(UTF8_TEXT, lambda: [text.encode('utf8')])


def maybe_wrap(wrapper, func):
    """Merge metadata for func into wrapper if functools is present."""
    if functools is not None:
        wrapper = functools.update_wrapper(wrapper, func)
    return wrapper


def content_from_file(path, content_type=None, chunk_size=DEFAULT_CHUNK_SIZE,
                      buffer_now=False, seek_offset=None, seek_whence=0):
    """Create a Content object from a file on disk.

    Note that unless ``buffer_now`` is explicitly passed in as True, the file
    will only be read from when ``iter_bytes`` is called.

    :param path: The path to the file to be used as content.
    :param content_type: The type of content.  If not specified, defaults
        to UTF8-encoded text/plain.
    :param chunk_size: The size of chunks to read from the file.
        Defaults to ``DEFAULT_CHUNK_SIZE``.
    :param buffer_now: If True, read the file from disk now and keep it in
        memory. Otherwise, only read when the content is serialized.
    :param seek_offset: If non-None, seek within the stream before reading it.
    :param seek_whence: If supplied, pass to ``stream.seek()`` when seeking.
    """
    if content_type is None:
        content_type = UTF8_TEXT
    def reader():
        with open(path, 'rb') as stream:
            yield from _iter_chunks(stream,
                                      chunk_size,
                                      seek_offset,
                                      seek_whence)
    return content_from_reader(reader, content_type, buffer_now)


def content_from_stream(stream, content_type=None,
                        chunk_size=DEFAULT_CHUNK_SIZE, buffer_now=False,
                        seek_offset=None, seek_whence=0):
    """Create a Content object from a file-like stream.

    Note that unless ``buffer_now`` is explicitly passed in as True, the stream
    will only be read from when ``iter_bytes`` is called.

    :param stream: A file-like object to read the content from. The stream
        is not closed by this function or the 'Content' object it returns.
    :param content_type: The type of content. If not specified, defaults
        to UTF8-encoded text/plain.
    :param chunk_size: The size of chunks to read from the file.
        Defaults to ``DEFAULT_CHUNK_SIZE``.
    :param buffer_now: If True, reads from the stream right now. Otherwise,
        only reads when the content is serialized. Defaults to False.
    :param seek_offset: If non-None, seek within the stream before reading it.
    :param seek_whence: If supplied, pass to ``stream.seek()`` when seeking.
    """
    if content_type is None:
        content_type = UTF8_TEXT
    def reader():
        return _iter_chunks(stream, chunk_size, seek_offset, seek_whence)
    return content_from_reader(reader, content_type, buffer_now)


def content_from_reader(reader, content_type, buffer_now):
    """Create a Content object that will obtain the content from reader.

    :param reader: A callback to read the content. Should return an iterable of
        bytestrings.
    :param content_type: The content type to create.
    :param buffer_now: If True the reader is evaluated immediately and
        buffered.
    """
    if content_type is None:
        content_type = UTF8_TEXT
    if buffer_now:
        contents = list(reader())
        def reader():
            return contents
    return Content(content_type, reader)


def attach_file(detailed, path, name=None, content_type=None,
                chunk_size=DEFAULT_CHUNK_SIZE, buffer_now=True):
    """Attach a file to this test as a detail.

    This is a convenience method wrapping around ``addDetail``.

    Note that by default the contents of the file will be read immediately. If
    ``buffer_now`` is False, then the file *must* exist when the test result is
    called with the results of this test, after the test has been torn down.

    :param detailed: An object with details
    :param path: The path to the file to attach.
    :param name: The name to give to the detail for the attached file.
    :param content_type: The content type of the file.  If not provided,
        defaults to UTF8-encoded text/plain.
    :param chunk_size: The size of chunks to read from the file.  Defaults
        to something sensible.
    :param buffer_now: If False the file content is read when the content
        object is evaluated rather than when attach_file is called.
        Note that this may be after any cleanups that obj_with_details has, so
        if the file is a temporary file disabling buffer_now may cause the file
        to be read after it is deleted. To handle those cases, using
        attach_file as a cleanup is recommended because it guarantees a
        sequence for when the attach_file call is made::

            detailed.addCleanup(attach_file, 'foo.txt', detailed)
    """
    if name is None:
        name = os.path.basename(path)
    content_object = content_from_file(
        path, content_type, chunk_size, buffer_now)
    detailed.addDetail(name, content_object)
