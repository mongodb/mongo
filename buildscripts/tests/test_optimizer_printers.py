"""Unit tests for optimizer GDB pretty-printer helpers."""

import importlib
import sys
import types
import unittest
from unittest.mock import Mock, patch


class TestOptimizerPrinterHelpers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fake_gdb = types.ModuleType("gdb")
        fake_gdb.Type = object
        fake_gdb.Symbol = object
        fake_gdb.Value = object
        fake_gdb.error = RuntimeError
        fake_gdb.GdbError = RuntimeError
        fake_gdb.TYPE_CODE_REF = object()
        fake_gdb_printing = types.ModuleType("gdb.printing")
        fake_gdb.printing = fake_gdb_printing

        with patch.dict(
            sys.modules,
            {"gdb": fake_gdb, "gdb.printing": fake_gdb_printing},
        ):
            cls.optimizer_printers = importlib.import_module("buildscripts.gdb.optimizer_printers")

    def test_formats_sbe_value_from_gdb_string(self):
        self.assertEqual(
            self.optimizer_printers._format_sbe_printed_value('"tag: NumberInt32, val: 2"'),
            '"2"',
        )

    def test_print_sbe_value_uses_exact_function_signature(self):
        print_fn = Mock(return_value='"tag: NumberInt32, val: 2"')
        with patch.object(
            self.optimizer_printers, "lookup_symbol", return_value=print_fn
        ) as lookup:
            result = self.optimizer_printers.ConstantPrinter.print_sbe_value(1, 2)

        self.assertEqual(result, '"2"')
        lookup.assert_called_once_with(
            "mongo::sbe::value::printTagAndVal",
            "std::string (*)(unsigned char, unsigned long)",
            function_signatures=(
                "mongo::sbe::value::printTagAndVal"
                "(mongo::sbe::value::TypeTags, mongo::sbe::value::Value)",
                "mongo::sbe::value::printTagAndVal" "(mongo::sbe::value::TypeTags, unsigned long)",
            ),
        )
        print_fn.assert_called_once_with(1, 2)


if __name__ == "__main__":
    unittest.main()
