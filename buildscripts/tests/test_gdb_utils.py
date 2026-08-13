"""Unit tests for the GDB helper functions."""

import importlib
import sys
import types
import unittest
from unittest.mock import Mock, call, patch


class TestLookupSymbol(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fake_gdb = types.ModuleType("gdb")
        fake_gdb.Type = object
        fake_gdb.Symbol = object
        fake_gdb.Value = object
        fake_gdb.TYPE_CODE_FUNC = 1
        fake_gdb.error = RuntimeError
        fake_gdb.execute = Mock(return_value="")
        cls.gdb = fake_gdb

        with patch.dict(sys.modules, {"gdb": fake_gdb}):
            cls.mongo_utils = importlib.import_module("buildscripts.gdb.mongo_utils")

    def setUp(self):
        self.mongo_utils.MAIN_GLOBAL_BLOCK = None
        self.gdb.execute = Mock(return_value="")
        self.gdb.Value = Mock()
        self.gdb.parse_and_eval = Mock()

    def test_returns_symbol_from_context_lookup(self):
        symbol = object()
        self.gdb.lookup_symbol = Mock(return_value=(symbol, False))
        self.gdb.objfiles = Mock()

        result = self.mongo_utils.lookup_symbol("function")

        self.assertIs(result, symbol)
        self.gdb.lookup_symbol.assert_called_once_with("function")
        self.gdb.objfiles.assert_not_called()

    def test_falls_back_to_loaded_objfiles(self):
        symbol = object()
        first_objfile = Mock()
        first_objfile.lookup_global_symbol.return_value = None
        second_objfile = Mock()
        second_objfile.lookup_global_symbol.return_value = symbol
        self.gdb.lookup_symbol = Mock(return_value=(None, False))
        self.gdb.objfiles = Mock(return_value=[first_objfile, second_objfile])

        result = self.mongo_utils.lookup_symbol("function")

        self.assertIs(result, symbol)
        first_objfile.lookup_global_symbol.assert_called_once_with("function")
        second_objfile.lookup_global_symbol.assert_called_once_with("function")

    def test_returns_none_when_symbol_is_not_found(self):
        objfile = Mock()
        objfile.lookup_global_symbol.return_value = None
        self.gdb.lookup_symbol = Mock(return_value=(None, False))
        self.gdb.objfiles = Mock(return_value=[objfile])

        self.assertIsNone(self.mongo_utils.lookup_symbol("function"))

    def test_falls_back_to_non_debugging_function_symbol(self):
        symbol = object()
        objfile = Mock()
        objfile.lookup_global_symbol.return_value = None
        self.gdb.lookup_symbol = Mock(return_value=(None, False))
        self.gdb.objfiles = Mock(return_value=[objfile])
        self.gdb.execute = Mock(
            return_value=(
                "Non-debugging symbols:\n"
                "0x1234 mongo::sbe::value::print[abi:cxx11]("
                "std::pair<mongo::sbe::value::TypeTags, unsigned long> const&)\n"
                "0x5678 mongo::sbe::value::printTagAndVal[abi:cxx11]("
                "mongo::sbe::value::TypeTags, unsigned long)\n"
            )
        )
        self.gdb.parse_and_eval = Mock(side_effect=RuntimeError("cannot resolve function"))
        self.gdb.Value = Mock(return_value=symbol)

        result = self.mongo_utils.lookup_symbol("mongo::sbe::value::print")

        self.assertIs(result, symbol)
        self.gdb.Value.assert_called_once_with(0x1234)

    def test_casts_non_debugging_function_symbol(self):
        typed_symbol = object()
        address_value = Mock()
        address_value.cast.return_value.dereference.return_value = typed_symbol
        pointer_type = Mock()
        type_value = Mock(type=pointer_type)
        objfile = Mock()
        objfile.lookup_global_symbol.return_value = None
        self.gdb.lookup_symbol = Mock(return_value=(None, False))
        self.gdb.objfiles = Mock(return_value=[objfile])
        self.gdb.execute = Mock(
            return_value="0x1234 mongo::sbe::value::printTagAndVal[abi:cxx11](...)\n"
        )
        self.gdb.parse_and_eval = Mock(
            side_effect=[
                RuntimeError("cannot resolve function"),
                RuntimeError("cannot resolve ABI-less function"),
                type_value,
            ]
        )
        self.gdb.Value = Mock(return_value=address_value)

        result = self.mongo_utils.lookup_symbol(
            "mongo::sbe::value::printTagAndVal",
            "std::string (*)(unsigned char, unsigned long)",
        )

        self.assertIs(result, typed_symbol)
        self.gdb.Value.assert_called_once_with(0x1234)
        address_value.cast.assert_called_once_with(pointer_type)
        self.gdb.parse_and_eval.assert_has_calls(
            [
                call("'mongo::sbe::value::printTagAndVal[abi:cxx11](...)'"),
                call("'mongo::sbe::value::printTagAndVal(...)'"),
                call("(std::string (*)(unsigned char, unsigned long))0"),
            ]
        )

    def test_prefers_exact_abi_tagged_function_expression(self):
        typed_symbol = Mock()
        typed_symbol.value = None
        typed_symbol.type.code = self.gdb.TYPE_CODE_FUNC
        objfile = Mock()
        objfile.lookup_global_symbol.return_value = None
        self.gdb.lookup_symbol = Mock(return_value=(None, False))
        self.gdb.objfiles = Mock(return_value=[objfile])
        self.gdb.execute = Mock(
            return_value=(
                "0x1234 mongo::sbe::value::printTagAndVal[abi:cxx11]("
                "mongo::sbe::value::TypeTags, unsigned long)\n"
            )
        )
        self.gdb.parse_and_eval = Mock(return_value=typed_symbol)

        result = self.mongo_utils.lookup_symbol(
            "mongo::sbe::value::printTagAndVal",
            "std::string (*)(unsigned char, unsigned long)",
        )

        self.assertIs(result, typed_symbol)
        self.gdb.parse_and_eval.assert_called_once_with(
            "'mongo::sbe::value::printTagAndVal[abi:cxx11]("
            "mongo::sbe::value::TypeTags, unsigned long)'"
        )
        self.gdb.Value.assert_not_called()

    def test_falls_back_to_info_address_for_non_debugging_function_symbol(self):
        typed_symbol = object()
        pointer_type = Mock()
        type_value = Mock(type=pointer_type)
        address_value = Mock()
        address_value.cast.return_value.dereference.return_value = typed_symbol
        objfile = Mock()
        objfile.lookup_global_symbol.return_value = None
        self.gdb.lookup_symbol = Mock(return_value=(None, False))
        self.gdb.objfiles = Mock(return_value=[objfile])
        self.gdb.execute = Mock(
            side_effect=[
                'Symbol "mongo::sbe::value::printTagAndVal(...)" is at '
                "0x1234 in a file compiled without debugging.",
                "",
            ]
        )
        self.gdb.parse_and_eval = Mock(return_value=type_value)
        self.gdb.Value = Mock(return_value=address_value)

        result = self.mongo_utils.lookup_symbol(
            "mongo::sbe::value::printTagAndVal",
            "std::string (*)(unsigned char, unsigned long)",
            function_signatures=(
                "mongo::sbe::value::printTagAndVal" "(mongo::sbe::value::TypeTags, unsigned long)",
            ),
        )

        self.assertIs(result, typed_symbol)
        self.gdb.execute.assert_any_call(
            "info address mongo::sbe::value::printTagAndVal"
            "(mongo::sbe::value::TypeTags, unsigned long)",
            to_string=True,
        )
        self.gdb.Value.assert_called_once_with(0x1234)
        address_value.cast.assert_called_once_with(pointer_type)

    def test_info_address_accepts_debug_function_address_output(self):
        typed_symbol = object()
        pointer_type = Mock()
        type_value = Mock(type=pointer_type)
        address_value = Mock()
        address_value.cast.return_value.dereference.return_value = typed_symbol
        objfile = Mock()
        objfile.lookup_global_symbol.return_value = None
        self.gdb.lookup_symbol = Mock(return_value=(None, False))
        self.gdb.objfiles = Mock(return_value=[objfile])
        self.gdb.execute = Mock(
            side_effect=[
                'Symbol "mongo::sbe::value::printTagAndVal(...)" is a function at address '
                "0x1234.",
                "",
            ]
        )
        self.gdb.parse_and_eval = Mock(return_value=type_value)
        self.gdb.Value = Mock(return_value=address_value)

        result = self.mongo_utils.lookup_symbol(
            "mongo::sbe::value::printTagAndVal",
            "std::string (*)(unsigned char, unsigned long)",
            function_signatures=("mongo::sbe::value::printTagAndVal(...)",),
        )

        self.assertIs(result, typed_symbol)
        self.gdb.Value.assert_called_once_with(0x1234)

    def test_info_address_uses_exact_signature_when_cast_fails(self):
        typed_symbol = Mock()
        typed_symbol.value = None
        typed_symbol.type.code = self.gdb.TYPE_CODE_FUNC
        objfile = Mock()
        objfile.lookup_global_symbol.return_value = None
        self.gdb.lookup_symbol = Mock(return_value=(None, False))
        self.gdb.objfiles = Mock(return_value=[objfile])
        function_signature = (
            "mongo::sbe::value::printTagAndVal" "(mongo::sbe::value::TypeTags, unsigned long)"
        )
        self.gdb.execute = Mock(
            return_value='Symbol "mongo::sbe::value::printTagAndVal(...)" is at 0x1234.'
        )
        self.gdb.parse_and_eval = Mock(
            side_effect=[RuntimeError("cannot resolve std::string"), typed_symbol]
        )

        result = self.mongo_utils.lookup_symbol(
            "mongo::sbe::value::printTagAndVal",
            "std::string (*)(unsigned char, unsigned long)",
            function_signatures=(function_signature,),
        )

        self.assertIs(result, typed_symbol)
        self.gdb.parse_and_eval.assert_has_calls(
            [
                call("(std::string (*)(unsigned char, unsigned long))0"),
                call("'" + function_signature + "'"),
            ]
        )
        self.gdb.Value.assert_not_called()

    def test_info_address_tries_each_function_signature(self):
        typed_symbol = object()
        pointer_type = Mock()
        type_value = Mock(type=pointer_type)
        address_value = Mock()
        address_value.cast.return_value.dereference.return_value = typed_symbol
        objfile = Mock()
        objfile.lookup_global_symbol.return_value = None
        self.gdb.lookup_symbol = Mock(return_value=(None, False))
        self.gdb.objfiles = Mock(return_value=[objfile])
        self.gdb.execute = Mock(
            side_effect=[
                RuntimeError("No symbol with typedef spelling"),
                'Symbol "mongo::sbe::value::printTagAndVal(...)" is at 0x5678.',
            ]
        )
        self.gdb.parse_and_eval = Mock(return_value=type_value)
        self.gdb.Value = Mock(return_value=address_value)
        typedef_signature = (
            "mongo::sbe::value::printTagAndVal"
            "(mongo::sbe::value::TypeTags, mongo::sbe::value::Value)"
        )
        underlying_signature = (
            "mongo::sbe::value::printTagAndVal" "(mongo::sbe::value::TypeTags, unsigned long)"
        )

        result = self.mongo_utils.lookup_symbol(
            "mongo::sbe::value::printTagAndVal",
            "std::string (*)(unsigned char, unsigned long)",
            function_signatures=(typedef_signature, underlying_signature),
        )

        self.assertIs(result, typed_symbol)
        self.gdb.execute.assert_has_calls(
            [
                call("info address " + typedef_signature, to_string=True),
                call("info address " + underlying_signature, to_string=True),
            ]
        )
        self.gdb.Value.assert_called_once_with(0x5678)

    def test_info_address_survives_dwarf_errors_from_python_lookups(self):
        typed_symbol = object()
        pointer_type = Mock()
        type_value = Mock(type=pointer_type)
        address_value = Mock()
        address_value.cast.return_value.dereference.return_value = typed_symbol
        objfile = Mock()
        objfile.lookup_global_symbol.side_effect = RuntimeError(
            "DWARF Error: unexpected tag 'DW_TAG_skeleton_unit'"
        )
        self.gdb.lookup_symbol = Mock(
            side_effect=RuntimeError("DWARF Error: unexpected tag 'DW_TAG_skeleton_unit'")
        )
        self.gdb.objfiles = Mock(return_value=[objfile])
        self.gdb.execute = Mock(
            return_value='Symbol "mongo::sbe::value::printTagAndVal(...)" is at 0x5678.'
        )
        self.gdb.parse_and_eval = Mock(return_value=type_value)
        self.gdb.Value = Mock(return_value=address_value)

        result = self.mongo_utils.lookup_symbol(
            "mongo::sbe::value::printTagAndVal",
            "std::string (*)(unsigned char, unsigned long)",
            function_signatures=("mongo::sbe::value::printTagAndVal(...)",),
        )

        self.assertIs(result, typed_symbol)
        objfile.lookup_global_symbol.assert_called_once_with("mongo::sbe::value::printTagAndVal")
        self.gdb.execute.assert_called_once_with(
            "info address mongo::sbe::value::printTagAndVal(...)",
            to_string=True,
        )

    def test_info_address_survives_dwarf_errors_while_casting_python_symbols(self):
        typed_symbol = object()
        pointer_type = Mock()
        type_value = Mock(type=pointer_type)
        address_value = Mock()
        address_value.cast.return_value.dereference.return_value = typed_symbol
        context_symbol = Mock()
        context_symbol.value.side_effect = RuntimeError(
            "DWARF Error: unexpected tag 'DW_TAG_skeleton_unit'"
        )
        objfile_symbol = Mock()
        objfile_symbol.value.side_effect = RuntimeError(
            "DWARF Error: unexpected tag 'DW_TAG_skeleton_unit'"
        )
        objfile = Mock()
        objfile.lookup_global_symbol.return_value = objfile_symbol
        self.gdb.lookup_symbol = Mock(return_value=(context_symbol, False))
        self.gdb.objfiles = Mock(return_value=[objfile])
        self.gdb.execute = Mock(
            return_value='Symbol "mongo::sbe::value::printTagAndVal(...)" is at 0x5678.'
        )
        self.gdb.parse_and_eval = Mock(return_value=type_value)
        self.gdb.Value = Mock(return_value=address_value)

        result = self.mongo_utils.lookup_symbol(
            "mongo::sbe::value::printTagAndVal",
            "std::string (*)(unsigned char, unsigned long)",
            function_signatures=("mongo::sbe::value::printTagAndVal(...)",),
        )

        self.assertIs(result, typed_symbol)
        objfile.lookup_global_symbol.assert_called_once_with("mongo::sbe::value::printTagAndVal")
        self.gdb.execute.assert_called_once_with(
            "info address mongo::sbe::value::printTagAndVal(...)",
            to_string=True,
        )


if __name__ == "__main__":
    unittest.main()
