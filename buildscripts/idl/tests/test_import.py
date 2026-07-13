#!/usr/bin/env python3
# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""Test cases for IDL binder."""

import io
import textwrap
import unittest

# import package so that it works regardless of whether we run as a module or file
if __package__ is None:
    import sys
    from os import path

    sys.path.append(path.dirname(path.abspath(__file__)))
    import testcase
    from context import idl
else:
    from . import testcase
    from .context import idl


class DictionaryImportResolver(idl.parser.ImportResolverBase):
    """An import resolver resolves files from a dictionary."""

    def __init__(self, import_dict):
        # type: (Dict[str, str]) -> None
        """Construct a DictionaryImportResolver."""
        self._import_dict = import_dict
        super(DictionaryImportResolver, self).__init__()

    def resolve(self, base_file, imported_file_name):
        # type: (str, str) -> str
        """Return the complete path to an imported file name."""
        if imported_file_name not in self._import_dict:
            return None

        return "imported_%s" % (imported_file_name)

    def open(self, resolved_file_name):
        # type: (str) -> Any
        """Return an io.Stream for the requested file."""
        assert resolved_file_name.startswith("imported_")
        imported_file_name = resolved_file_name.replace("imported_", "")

        return io.StringIO(self._import_dict[imported_file_name])


class TestImport(testcase.IDLTestcase):
    """Test cases for the IDL binder."""

    # Test: import wrong types
    def test_import_negative_parser(self):
        # type: () -> None
        """Negative import parser tests."""

        self.assert_parse_fail(
            textwrap.dedent("""
        imports:
            - "a.idl"

        imports:
            - "b.idl"
            """),
            idl.errors.ERROR_ID_DUPLICATE_NODE,
        )

        self.assert_parse_fail(
            textwrap.dedent("""
        imports: "basetypes.idl"
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        self.assert_parse_fail(
            textwrap.dedent("""
        imports:
            a: "a.idl"
            b: "b.idl"
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

    def test_import_positive(self):
        # type: () -> None
        """Postive import tests."""

        import_dict = {
            "basetypes.idl": textwrap.dedent("""
            global:
                cpp_namespace: 'mongo'

            types:
                string:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: string
                    serializer: foo
                    deserializer: foo
                    default: foo
                    is_view: false
                serialization_context:
                    bson_serialization_type: any
                    description: foo
                    cpp_type: foo
                    internal_only: true
                    is_view: false

            structs:
                bar:
                    description: foo
                    strict: false
                    fields:
                        foo: string
            """),
            "recurse1.idl": textwrap.dedent("""
            imports:
                - "basetypes.idl"

            types:
                int:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: int
                    deserializer: BSONElement::fake
                    is_view: false

            """),
            "recurse2.idl": textwrap.dedent("""
            imports:
                - "recurse1.idl"

            types:
                double:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: double
                    deserializer: BSONElement::fake
                    is_view: false

            """),
            "recurse1b.idl": textwrap.dedent("""
            imports:
                - "basetypes.idl"

            types:
                bool:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: bool
                    deserializer: BSONElement::fake
                    is_view: false
            """),
            "cycle1a.idl": textwrap.dedent("""
            global:
                cpp_namespace: 'mongo'

            imports:
                - "cycle1b.idl"

            types:
                string:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: string
                    serializer: foo
                    deserializer: foo
                    default: foo
                    is_view: false
                serialization_context:
                    bson_serialization_type: any
                    description: foo
                    cpp_type: foo
                    internal_only: true
                    is_view: false

            structs:
                bar:
                    description: foo
                    strict: false
                    fields:
                        foo: string
                        foo1: bool
            """),
            "cycle1b.idl": textwrap.dedent("""
            global:
                cpp_namespace: 'mongo'

            imports:
                - "cycle1a.idl"

            types:
                bool:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: bool
                    deserializer: BSONElement::fake
                    is_view: false

            structs:
                bar2:
                    description: foo
                    strict: false
                    fields:
                        foo: string
                        foo1: bool
            """),
            "cycle2.idl": textwrap.dedent("""
            global:
                cpp_namespace: 'mongo'

            imports:
                - "cycle2.idl"

            types:
                string:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: string
                    serializer: foo
                    deserializer: foo
                    default: foo
                    is_view: false
                serialization_context:
                    bson_serialization_type: any
                    description: foo
                    cpp_type: foo
                    internal_only: true
                    is_view: false
            """),
        }

        resolver = DictionaryImportResolver(import_dict)

        # Test simple import
        self.assert_bind(
            textwrap.dedent("""
        global:
            cpp_namespace: 'mongo'

        imports:
            - "basetypes.idl"

        structs:
            foobar:
                description: foo
                strict: false
                fields:
                    foo: string
            """),
            resolver=resolver,
        )

        # Test nested import
        self.assert_bind(
            textwrap.dedent("""
        global:
            cpp_namespace: 'mongo'

        imports:
            - "recurse2.idl"

        structs:
            foobar:
                description: foo
                strict: false
                fields:
                    foo: string
                    foo1: int
                    foo2: double
            """),
            resolver=resolver,
        )

        # Test diamond import
        self.assert_bind(
            textwrap.dedent("""
        global:
            cpp_namespace: 'mongo'

        imports:
            - "recurse2.idl"
            - "recurse1b.idl"

        structs:
            foobar:
                description: foo
                strict: false
                fields:
                    foo: string
                    foo1: int
                    foo2: double
                    foo3: bool
            """),
            resolver=resolver,
        )

        # Test cycle import
        self.assert_bind(
            textwrap.dedent("""
        global:
            cpp_namespace: 'mongo'

        imports:
            - "cycle1a.idl"

        structs:
            foobar:
                description: foo
                strict: false
                fields:
                    foo: string
                    foo1: bool
            """),
            resolver=resolver,
        )

        # Test self cycle import
        self.assert_bind(
            textwrap.dedent("""
        global:
            cpp_namespace: 'mongo'

        imports:
            - "cycle2.idl"

        structs:
            foobar:
                description: foo
                strict: false
                fields:
                    foo: string
            """),
            resolver=resolver,
        )

    def test_import_negative(self):
        # type: () -> None
        """Negative import tests."""

        import_dict = {
            "basetypes.idl": textwrap.dedent("""
            global:
                cpp_namespace: 'mongo'

            types:
                string:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: string
                    serializer: foo
                    deserializer: foo
                    default: foo
                    is_view: false

            structs:
                bar:
                    description: foo
                    strict: false
                    fields:
                        foo: string

            enums:
                IntEnum:
                    description: "An example int enum"
                    type: int
                    values:
                        a0: 0
                        b1: 1

            """),
            "bug.idl": textwrap.dedent("""
            global:
                cpp_namespace: 'mongo'

            types:
                bool:
                    description: foo
                    bson_serialization_type: bool
                    deserializer: BSONElement::fake
                    is_view: false
            """),
        }

        resolver = DictionaryImportResolver(import_dict)

        # Bad import
        self.assert_parse_fail(
            textwrap.dedent("""
        imports:
            - "notfound.idl"
            """),
            idl.errors.ERROR_ID_BAD_IMPORT,
            resolver=resolver,
        )

        # Import a file with errors
        self.assert_parse_fail(
            textwrap.dedent("""
        imports:
            - "basetypes.idl"
            - "bug.idl"

        types:
            string2:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                is_view: false
            """),
            idl.errors.ERROR_ID_MISSING_REQUIRED_FIELD,
            resolver=resolver,
        )


if __name__ == "__main__":
    unittest.main()
