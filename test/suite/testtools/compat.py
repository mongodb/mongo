# Copyright (c) 2008-2011 testtools developers. See LICENSE for details.

"""Compatibility support for python 2 and 3."""

__metaclass__ = type
__all__ = [
    '_b',
    '_u',
    'advance_iterator',
    'str_is_unicode',
    'StringIO',
    'unicode_output_stream',
    ]

import codecs
import linecache
import locale
import os
import re
import sys
import traceback

from testtools.helpers import try_imports

BytesIO = try_imports(['StringIO.StringIO', 'io.BytesIO'])
StringIO = try_imports(['StringIO.StringIO', 'io.StringIO'])


__u_doc = """A function version of the 'u' prefix.

This is needed becayse the u prefix is not usable in Python 3 but is required
in Python 2 to get a unicode object.

To migrate code that was written as u'\u1234' in Python 2 to 2+3 change
it to be _u('\u1234'). The Python 3 interpreter will decode it
appropriately and the no-op _u for Python 3 lets it through, in Python
2 we then call unicode-escape in the _u function.
"""

if sys.version_info > (3, 0):
    def _u(s):
        return s
    _r = ascii
    def _b(s):
        """A byte literal."""
        return s.encode("latin-1")
    advance_iterator = next
    def istext(x):
        return isinstance(x, str)
    def classtypes():
        return (type,)
    str_is_unicode = True
else:
    def _u(s):
        # The double replace mangling going on prepares the string for
        # unicode-escape - \foo is preserved, \u and \U are decoded.
        return (s.replace("\\", "\\\\").replace("\\\\u", "\\u")
            .replace("\\\\U", "\\U").decode("unicode-escape"))
    _r = repr
    def _b(s):
        return s
    advance_iterator = lambda it: it.next()
    def istext(x):
        return isinstance(x, basestring)
    def classtypes():
        import types
        return (type, types.ClassType)
    str_is_unicode = sys.platform == "cli"

_u.__doc__ = __u_doc


if sys.version_info > (2, 5):
    all = all
    _error_repr = BaseException.__repr__
    def isbaseexception(exception):
        """Return whether exception inherits from BaseException only"""
        return (isinstance(exception, BaseException)
            and not isinstance(exception, Exception))
else:
    def all(iterable):
        """If contents of iterable all evaluate as boolean True"""
        for obj in iterable:
            if not obj:
                return False
        return True
    def _error_repr(exception):
        """Format an exception instance as Python 2.5 and later do"""
        return exception.__class__.__name__ + repr(exception.args)
    def isbaseexception(exception):
        """Return whether exception would inherit from BaseException only

        This approximates the hierarchy in Python 2.5 and later, compare the
        difference between the diagrams at the bottom of the pages:
        <http://docs.python.org/release/2.4.4/lib/module-exceptions.html>
        <http://docs.python.org/release/2.5.4/lib/module-exceptions.html>
        """
        return isinstance(exception, (KeyboardInterrupt, SystemExit))


def unicode_output_stream(stream):
    """Get wrapper for given stream that writes any unicode without exception

    Characters that can't be coerced to the encoding of the stream, or 'ascii'
    if valid encoding is not found, will be replaced. The original stream may
    be returned in situations where a wrapper is determined unneeded.

    The wrapper only allows unicode to be written, not non-ascii bytestrings,
    which is a good thing to ensure sanity and sanitation.
    """
    if sys.platform == "cli":
        # Best to never encode before writing in IronPython
        return stream
    try:
        writer = codecs.getwriter(stream.encoding or "")
    except (AttributeError, LookupError):
        # GZ 2010-06-16: Python 3 StringIO ends up here, but probably needs
        #                different handling as it doesn't want bytestrings
        return codecs.getwriter("ascii")(stream, "replace")
    if writer.__module__.rsplit(".", 1)[1].startswith("utf"):
        # The current stream has a unicode encoding so no error handler is needed
        return stream
    if sys.version_info > (3, 0):
        # Python 3 doesn't seem to make this easy, handle a common case
        try:
            return stream.__class__(stream.buffer, stream.encoding, "replace",
                stream.newlines, stream.line_buffering)
        except AttributeError:
            pass
    return writer(stream, "replace")    


# The default source encoding is actually "iso-8859-1" until Python 2.5 but
# using non-ascii causes a deprecation warning in 2.4 and it's cleaner to
# treat all versions the same way
_default_source_encoding = "ascii"

# Pattern specified in <http://www.python.org/dev/peps/pep-0263/>
_cookie_search=re.compile("coding[:=]\s*([-\w.]+)").search

def _detect_encoding(lines):
    """Get the encoding of a Python source file from a list of lines as bytes

    This function does less than tokenize.detect_encoding added in Python 3 as
    it does not attempt to raise a SyntaxError when the interpreter would, it
    just wants the encoding of a source file Python has already compiled and
    determined is valid.
    """
    if not lines:
        return _default_source_encoding
    if lines[0].startswith("\xef\xbb\xbf"):
        # Source starting with UTF-8 BOM is either UTF-8 or a SyntaxError
        return "utf-8"
    # Only the first two lines of the source file are examined
    magic = _cookie_search("".join(lines[:2]))
    if magic is None:
        return _default_source_encoding
    encoding = magic.group(1)
    try:
        codecs.lookup(encoding)
    except LookupError:
        # Some codecs raise something other than LookupError if they don't
        # support the given error handler, but not the text ones that could
        # actually be used for Python source code
        return _default_source_encoding
    return encoding


class _EncodingTuple(tuple):
    """A tuple type that can have an encoding attribute smuggled on"""


def _get_source_encoding(filename):
    """Detect, cache and return the encoding of Python source at filename"""
    try:
        return linecache.cache[filename].encoding
    except (AttributeError, KeyError):
        encoding = _detect_encoding(linecache.getlines(filename))
        if filename in linecache.cache:
            newtuple = _EncodingTuple(linecache.cache[filename])
            newtuple.encoding = encoding
            linecache.cache[filename] = newtuple
        return encoding


def _get_exception_encoding():
    """Return the encoding we expect messages from the OS to be encoded in"""
    if os.name == "nt":
        # GZ 2010-05-24: Really want the codepage number instead, the error
        #                handling of standard codecs is more deterministic
        return "mbcs"
    # GZ 2010-05-23: We need this call to be after initialisation, but there's
    #                no benefit in asking more than once as it's a global
    #                setting that can change after the message is formatted.
    return locale.getlocale(locale.LC_MESSAGES)[1] or "ascii"


def _exception_to_text(evalue):
    """Try hard to get a sensible text value out of an exception instance"""
    try:
        return unicode(evalue)
    except KeyboardInterrupt:
        raise
    except:
        # Apparently this is what traceback._some_str does. Sigh - RBC 20100623
        pass
    try:
        return str(evalue).decode(_get_exception_encoding(), "replace")
    except KeyboardInterrupt:
        raise
    except:
        # Apparently this is what traceback._some_str does. Sigh - RBC 20100623
        pass
    # Okay, out of ideas, let higher level handle it
    return None


# GZ 2010-05-23: This function is huge and horrible and I welcome suggestions
#                on the best way to break it up
_TB_HEADER = _u('Traceback (most recent call last):\n')
def _format_exc_info(eclass, evalue, tb, limit=None):
    """Format a stack trace and the exception information as unicode

    Compatibility function for Python 2 which ensures each component of a
    traceback is correctly decoded according to its origins.

    Based on traceback.format_exception and related functions.
    """
    fs_enc = sys.getfilesystemencoding()
    if tb:
        list = [_TB_HEADER]
        extracted_list = []
        for filename, lineno, name, line in traceback.extract_tb(tb, limit):
            extracted_list.append((
                filename.decode(fs_enc, "replace"),
                lineno,
                name.decode("ascii", "replace"),
                line and line.decode(
                    _get_source_encoding(filename), "replace")))
        list.extend(traceback.format_list(extracted_list))
    else:
        list = []
    if evalue is None:
        # Is a (deprecated) string exception
        list.append((eclass + "\n").decode("ascii", "replace"))
        return list
    if isinstance(evalue, SyntaxError):
        # Avoid duplicating the special formatting for SyntaxError here,
        # instead create a new instance with unicode filename and line
        # Potentially gives duff spacing, but that's a pre-existing issue
        try:
            msg, (filename, lineno, offset, line) = evalue
        except (TypeError, ValueError):
            pass # Strange exception instance, fall through to generic code
        else:
            # Errors during parsing give the line from buffer encoded as
            # latin-1 or utf-8 or the encoding of the file depending on the
            # coding and whether the patch for issue #1031213 is applied, so
            # give up on trying to decode it and just read the file again
            if line:
                bytestr = linecache.getline(filename, lineno)
                if bytestr:
                    if lineno == 1 and bytestr.startswith("\xef\xbb\xbf"):
                        bytestr = bytestr[3:]
                    line = bytestr.decode(
                        _get_source_encoding(filename), "replace")
                    del linecache.cache[filename]
                else:
                    line = line.decode("ascii", "replace")
            if filename:
                filename = filename.decode(fs_enc, "replace")
            evalue = eclass(msg, (filename, lineno, offset, line))
            list.extend(traceback.format_exception_only(eclass, evalue))
            return list
    sclass = eclass.__name__
    svalue = _exception_to_text(evalue)
    if svalue:
        list.append("%s: %s\n" % (sclass, svalue))
    elif svalue is None:
        # GZ 2010-05-24: Not a great fallback message, but keep for the moment
        list.append("%s: <unprintable %s object>\n" % (sclass, sclass))
    else:
        list.append("%s\n" % sclass)
    return list
