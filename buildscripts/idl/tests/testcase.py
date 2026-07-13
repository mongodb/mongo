# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""Utility methods and classes for testing IDL passes."""

import io
import unittest

if __name__ == "testcase":
    import sys
    from os import path

    sys.path.append(path.dirname(path.dirname(path.abspath(__file__))))
    from context import idl
else:
    from .context import idl


def errors_to_str(errors):
    # type: (idl.errors.ParserErrorCollection) -> str
    """Dump the list of errors as a multiline text string."""
    if errors is not None:
        return "\n".join(errors.to_list())
    return "<empty>"


class NothingImportResolver(idl.parser.ImportResolverBase):
    """An import resolver that does nothing."""

    def resolve(self, base_file, imported_file_name):
        # type: (str, str) -> str
        """Return the complete path to an imported file name."""
        return imported_file_name

    def open(self, imported_file_name):
        # type: (str) -> Any
        """Return an empty io.Stream."""
        return io.StringIO("")


class IDLTestcase(unittest.TestCase):
    """IDL Test case base class."""

    def _parse(self, doc_str, resolver):
        # type: (str, idl.parser.ImportResolverBase) -> idl.syntax.IDLParsedSpec
        """Parse a document and throw a unittest failure if it fails to parse as a valid YAML document."""

        try:
            return idl.parser.parse(doc_str, "unknown", resolver)
        except:
            self.fail("Failed to parse document:\n%s" % (doc_str))

    def _assert_parse(self, doc_str, parsed_doc):
        # type: (str, idl.syntax.IDLParsedSpec) -> None
        """Assert a document parsed correctly by the IDL compiler and returned no errors."""
        self.assertIsNone(
            parsed_doc.errors,
            "Expected no parser errors\nFor document:\n%s\nReceived errors:\n\n%s"
            % (doc_str, errors_to_str(parsed_doc.errors)),
        )
        self.assertIsNotNone(parsed_doc.spec, "Expected a parsed doc")

    def assert_parse(self, doc_str, resolver=NothingImportResolver()):
        # type: (str, idl.parser.ImportResolverBase) -> None
        """Assert a document parsed correctly by the IDL compiler and returned no errors."""
        parsed_doc = self._parse(doc_str, resolver)
        self._assert_parse(doc_str, parsed_doc)

    def assert_parse_fail(
        self, doc_str, error_id, multiple=False, resolver=NothingImportResolver()
    ):
        # type: (str, str, bool, idl.parser.ImportResolverBase) -> None
        """
        Assert a document parsed correctly by the YAML parser, but not the by the IDL compiler.

        Asserts only one error is found in the document to make future IDL changes easier.
        """
        parsed_doc = self._parse(doc_str, resolver)

        self.assertIsNone(parsed_doc.spec, "Expected no parsed doc")
        self.assertIsNotNone(parsed_doc.errors, "Expected parser errors")

        # Assert that negative test cases are only testing one fault in a test.
        # This is impossible to assert for all tests though.
        self.assertTrue(
            multiple or parsed_doc.errors.count() == 1,
            "For document:\n%s\nExpected only error message '%s' but received multiple errors:\n\n%s"
            % (doc_str, error_id, errors_to_str(parsed_doc.errors)),
        )

        self.assertTrue(
            parsed_doc.errors.contains(error_id),
            "For document:\n%s\nExpected error message '%s' but received only errors:\n %s"
            % (doc_str, error_id, errors_to_str(parsed_doc.errors)),
        )

    def assert_bind(self, doc_str, resolver=NothingImportResolver()):
        # type: (str, idl.parser.ImportResolverBase) -> idl.ast.IDLBoundSpec
        """Assert a document parsed and bound correctly by the IDL compiler and returned no errors."""
        parsed_doc = self._parse(doc_str, resolver)
        self._assert_parse(doc_str, parsed_doc)

        bound_doc = idl.binder.bind(parsed_doc.spec)

        self.assertIsNone(
            bound_doc.errors,
            "Expected no binder errors\nFor document:\n%s\nReceived errors:\n\n%s"
            % (doc_str, errors_to_str(bound_doc.errors)),
        )
        self.assertIsNotNone(bound_doc.spec, "Expected a bound doc")

        return bound_doc.spec

    def assert_bind_fail(self, doc_str, error_id, multiple=False, resolver=NothingImportResolver()):
        # type: (str, str, bool, idl.parser.ImportResolverBase) -> None
        """
        Assert a document parsed correctly by the YAML parser and IDL parser, but not bound by the IDL binder.

        Asserts only one error is found in the document to make future IDL changes easier.
        """
        parsed_doc = self._parse(doc_str, resolver)
        self._assert_parse(doc_str, parsed_doc)

        bound_doc = idl.binder.bind(parsed_doc.spec)

        self.assertIsNone(bound_doc.spec, "Expected no bound doc\nFor document:\n%s\n" % (doc_str))
        self.assertIsNotNone(bound_doc.errors, "Expected binder errors")

        # Assert that negative test cases are only testing one fault in a test.
        # This is impossible to assert for all tests though.
        self.assertTrue(
            (multiple and bound_doc.errors.count() >= 1) or bound_doc.errors.count() == 1,
            "For document:\n%s\nExpected only error message '%s' but received multiple errors:\n\n%s"
            % (doc_str, error_id, errors_to_str(bound_doc.errors)),
        )

        self.assertTrue(
            bound_doc.errors.contains(error_id),
            "For document:\n%s\nExpected error message '%s' but received only errors:\n %s"
            % (doc_str, error_id, errors_to_str(bound_doc.errors)),
        )

    def assert_generate(self, doc_str, resolver=NothingImportResolver()):
        # type: (str, idl.parser.ImportResolverBase) -> Tuple[str,str]
        """Assert a document parsed, bound, and generated correctly by the IDL compiler."""
        spec = self.assert_bind(doc_str, resolver)

        header = idl.generator.generate_header_str(spec)
        source = idl.generator.generate_source_str(spec, "fake", "fake_header")

        return (header, source)
