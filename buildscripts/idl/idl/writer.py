# Copyright (C) 2018-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
#
"""Text Writing Utilites."""

# Number of spaces to indent code
_INDENT_SPACE_COUNT = 4


def _fill_spaces(count):
    # type: (int) -> str
    """Fill a string full of spaces."""
    fill = ""
    for _ in range(count * _INDENT_SPACE_COUNT):
        fill += " "

    return fill


def _indent_text(count, unindented_text):
    # type: (int, str) -> str
    """Indent each line of a multi-line string."""
    lines = unindented_text.splitlines()
    fill = _fill_spaces(count)
    return "\n".join(fill + line for line in lines)


def is_function(name):
    # type: (str) -> bool
    """
    Return True if a serializer/deserializer is function.

    A function is prefixed with '::' so that the IDL generated code calls it as a function instead
    of as a class method.
    """
    return name.startswith("::")


def get_method_name(name):
    # type: (str) -> str
    """Get a method name from a fully qualified method name."""
    pos = name.rfind("::")
    if pos == -1:
        return name
    return name[pos + 2 :]


def get_method_name_from_qualified_method_name(name):
    # type: (str) -> str
    """Get a method name from a fully qualified method name."""
    # TODO: in the future, we may want to support full-qualified calls to static methods
    # Strip the global prefix from enum functions
    if name.startswith("::"):
        name = name[2:]

    prefix = "mongo::"
    pos = name.find(prefix)
    if pos == -1:
        return name

    return name[len(prefix) :]


class IndentedTextWriter(object):
    """
    A simple class to manage writing indented lines of text.

    Supports both writing indented lines, and unindented lines.
    Use write_empty_line() instead of write_line('') to avoid lines
    full of blank spaces.
    """

    def __init__(self, stream):
        # type: (io.StringIO) -> None
        """Create an indented text writer."""
        self._stream = stream
        self._indent = 0

    def write_unindented_line(self, msg):
        # type: (str) -> None
        """Write an unindented line to the stream, no template formattin applied."""
        self._stream.write(msg)
        self._stream.write("\n")

    def indent(self):
        # type: () -> None
        """Indent the text by one level."""
        self._indent += 1

    def unindent(self):
        # type: () -> None
        """Unindent the text by one level."""
        assert self._indent > 0
        self._indent -= 1

    def write_line(self, msg):
        # type: (str) -> None
        """Write a line to the stream, no template formattin applied."""
        self._stream.write(_indent_text(self._indent, msg))
        self._stream.write("\n")

    def write_empty_line(self):
        # type: () -> None
        """Write a line to the stream."""
        self._stream.write("\n")


class WriterBlock(object):
    """Interface for block types below."""

    def __enter__(self):
        # type: () -> None
        """Open a block."""
        pass

    def __exit__(self, *args):
        # type: (*str) -> None
        """Close the block."""
        pass


class EmptyBlock(WriterBlock):
    """Do not generate an indented block."""

    def __init__(self):
        # type: () -> None
        """Create an empty block."""
        pass

    def __enter__(self):
        # type: () -> None
        """Do nothing."""
        pass

    def __exit__(self, *args):
        # type: (*str) -> None
        """Do nothing."""
        pass


class IndentedScopedBlock(WriterBlock):
    """Generate a block, template the parameters, and indent the contents."""

    def __init__(self, writer, opening, closing):
        # type: (IndentedTextWriter, str, str) -> None
        """Create a block."""
        self._writer = writer
        self._opening = opening
        self._closing = closing

    def __enter__(self):
        # type: () -> None
        """Write the beginning of the block and then indent."""
        self._writer.write_line(self._opening)
        self._writer.indent()

    def __exit__(self, *args):
        # type: (*str) -> None
        """Unindent the block and print the ending."""
        self._writer.unindent()
        self._writer.write_line(self._closing)


class NamespaceScopeBlock(WriterBlock):
    """Generate an unindented blocks for a list of namespaces, and do not indent the contents."""

    def __init__(
        self, indented_writer: IndentedTextWriter, namespaces: list[str], mod_vis_str: str = ""
    ):
        # type: (IndentedTextWriter, List[str]) -> None
        """Create a block."""
        self._writer = indented_writer
        self._namespaces = namespaces
        self._mod_vis_str = mod_vis_str

    def __enter__(self):
        # type: () -> None
        """Write the beginning of the block and do not indent."""
        for namespace in self._namespaces:
            self._writer.write_unindented_line(f"namespace {self._mod_vis_str}{namespace} {{")

    def __exit__(self, *args):
        # type: (*str) -> None
        """Write the end of the block and do not change indentation."""
        self._namespaces.reverse()

        for namespace in self._namespaces:
            self._writer.write_unindented_line(f"}}  // namespace {self._mod_vis_str}{namespace}")


class UnindentedBlock(WriterBlock):
    """Generate a block without indentation."""

    def __init__(self, writer, opening, closing):
        # type: (IndentedTextWriter, str, str) -> None
        """Create a block."""
        self._writer = writer
        self._opening = opening
        self._closing = closing

    def __enter__(self):
        # type: () -> None
        """Write the beginning of the block."""
        self._writer.write_unindented_line(self._opening)

    def __exit__(self, *args):
        # type: (*str) -> None
        """Write the ending of the block."""
        self._writer.write_unindented_line(self._closing)


class MultiBlock(WriterBlock):
    """Proxy container for a list of WriterBlocks."""

    def __init__(self, blocks):
        # type: (MultiBlock, List[WriterBlock]) -> None
        """Create a multi-block."""
        self._blocks = blocks

    def __enter__(self):
        # type: () -> None
        """Enter each block forwards."""
        for i in self._blocks:
            i.__enter__()

    def __exit__(self, *args):
        # type: (*str) -> None
        """And leave each block in reverse."""
        for i in reversed(self._blocks):
            i.__exit__(*args)


def _get_common_prefix(words):
    # type: (List[str]) -> str
    """Returns a common prefix for a set of strings.

    Returns empty string if there is no prefix or a empty string
    """
    empty_words = [lw for lw in words if len(lw) == 0]
    if empty_words:
        return ""

    first_letters = {w[0] for w in words}

    if len(first_letters) == 1:
        short_words = [lw for lw in words if len(lw) == 1]
        if short_words:
            return words[0][0]

        suffix_words = [flw[1:] for flw in words]

        return words[0][0] + _get_common_prefix(suffix_words)
    else:
        return ""


def gen_trie(words, writer, callback):
    # type: (List[str], IndentedTextWriter, Callable[[str], None]) -> None
    """
    Generate a trie for a list of strings.

    Takes a callback function that can used to generate code that processes a specific word in the trie.
    i.e. for ["abc", "def"], then callback() will be called twice, once for each string.
    """
    words = sorted(words)

    _gen_trie("", words, writer, callback)


def _gen_trie(prefix, words, writer, callback):
    # type: (str, List[str], IndentedTextWriter, Callable[[str], None]) -> None
    """
    Recursively generate a trie.

    Prefix is a common prefix for all the strings in words, can be empty string.
    """
    assert len(words) >= 1
    # No duplicate strings allowed
    assert len(words) == len(set(words))

    prefix_len = len(prefix)

    # Base case: one word
    if len(words) == 1:
        # Check remaining string is a string match
        word_to_check = prefix + words[0]
        suffix = words[0]
        suffix_len = len(suffix)

        predicate = (
            f"fieldName.size() == {len(word_to_check)} && "
            + f'std::char_traits<char>::compare(fieldName.data() + {prefix_len}, "{suffix}", {suffix_len}) == 0'
        )

        # If there is no trailing text, we just need to check length to validate we matched
        if suffix_len == 0:
            predicate = f"fieldName.size() == {len(word_to_check)}"

        # Optimization:
        # Checking strings of length 1 or even length is efficient. Strings of 3 byte length are
        # inefficient to check as they require two comparisons (1 uint16 and 1 uint8) but 4 byte
        # length strings require just 1. Since we know the field name is zero terminated, we can
        # just use memcmp and compare with the trailing null byte.
        elif suffix_len % 4 == 3:
            predicate = (
                f"fieldName.size() == {len(word_to_check)} && "
                + f' memcmp(fieldName.data() + {prefix_len}, "{suffix}\\0", {suffix_len + 1}) == 0'
            )

        with IndentedScopedBlock(writer, f"if ({predicate}) {{", "}"):
            callback(word_to_check)

        return

    # Handle the case where one word is a prefix of another
    # For instance, ["short", "shorter"] will eventually call this function with
    # (prefix = "short", ["", "er"]) as the tuple of prefix and list of words
    empty_words = [lw for lw in words if len(lw) == 0]
    if empty_words:
        word_to_check = prefix
        with IndentedScopedBlock(writer, f"if (fieldName.size() == {len(word_to_check)}) {{", "}"):
            callback(word_to_check)

    # Filter out empty words
    words = [lw for lw in words if len(lw) > 0]

    # Optimization for a common prefix
    # Example: ["word1", "word2"]
    # Instead of generating a trie to check for letters individually (i.e. ["w", "o", "r", "d"]),
    # we check for the prefix all at once ("word")
    gcp = _get_common_prefix(words)
    if len(gcp) > 1:
        gcp_len = len(gcp)
        suffix_words = [flw[gcp_len:] for flw in words]

        with IndentedScopedBlock(
            writer,
            f"if (fieldName.size() >= {gcp_len} && "
            + f'std::char_traits<char>::compare(fieldName.data() + {prefix_len}, "{gcp}", {gcp_len}) == 0) {{',
            "}",
        ):
            _gen_trie(prefix + gcp, suffix_words, writer, callback)

        return

    # Handle the main case for the trie
    # We have a list of non-empty words with no common prefix between them,
    # the first letters among the words may contain duplicates
    sorted_words = sorted(words)
    first_letters = sorted(list({w[0] for w in sorted_words}))
    min_len = len(prefix) + min([len(w) for w in sorted_words])

    with IndentedScopedBlock(writer, f"if (fieldName.size() >= {min_len}) {{", "}"):
        first_if = True

        for first_letter in first_letters:
            fl_words = [flw[1:] for flw in words if flw[0] == first_letter]

            ei = "else " if not first_if else ""
            with IndentedScopedBlock(
                writer, f"{ei}if (fieldName[{len(prefix)}] == '{first_letter}') {{", "}"
            ):
                _gen_trie(prefix + first_letter, fl_words, writer, callback)

            first_if = False


def gen_string_table_find_function_block(out, in_str, on_match, on_fail, words):
    # type: (IndentedTextWriter, str, str, str, list[str]) -> None
    """Wrap a gen_trie generated block as a function."""
    index = {word: i for i, word in enumerate(words)}
    out.write_line(f"StringData fieldName{{{in_str}}};")
    gen_trie(words, out, lambda w: out.write_line(f"return {on_match.format(index[w])};"))
    out.write_line(f"return {on_fail};")
