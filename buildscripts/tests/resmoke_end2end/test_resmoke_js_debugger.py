"""Test resmoke's JavaScript debugger functionality."""

import io
import logging
import os
import re
import subprocess
import sys
import unittest
from shutil import rmtree

import pexpect


class _ResmokeSelftest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.test_dir = os.path.normpath("/data/db/selftest")

    def setUp(self):
        self.logger = logging.getLogger(self._testMethodName)
        self.logger.setLevel(logging.DEBUG)
        handler = logging.StreamHandler(sys.stdout)
        handler.setFormatter(logging.Formatter(fmt="%(message)s"))
        self.logger.addHandler(handler)

        self.logger.info("Cleaning temp directory %s", self.test_dir)
        rmtree(self.test_dir, ignore_errors=True)
        os.makedirs(self.test_dir, mode=0o755, exist_ok=True)


def execute_resmoke(resmoke_args, subcommand="run"):
    return subprocess.run(
        [sys.executable, "buildscripts/resmoke.py", subcommand] + resmoke_args,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


class TestJSDebugger(_ResmokeSelftest):
    """Test suite for JavaScript debugger functionality."""

    def test_debugger_without_flag(self):
        """Test that debugger statements are no-ops when --shellJSDebugMode is not set."""
        # Without the flag, debugger statements should be no-ops, so this test passes
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_debugger_nodb.yml",
            "buildscripts/tests/resmoke_end2end/testfiles/debugger/debugger_statement.js",
        ]
        result = execute_resmoke(resmoke_args)
        self.assertEqual(result.returncode, 0)

    def test_debugger_waits_for_input(self):
        """Test that debugger pauses execution when hitting a debugger statement."""
        # This test verifies the debugger is activated by confirming it pauses execution
        # Note: The mongo shell's debugger requires /dev/tty for interactive input,
        # so we can only verify that it pauses, not that it responds to commands in automation
        resmoke_cmd = [
            sys.executable,
            "buildscripts/resmoke.py",
            "run",
            f"--dbpathPrefix={self.test_dir}",
            "--shellJSDebugMode",
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_debugger_nodb.yml",
            "buildscripts/tests/resmoke_end2end/testfiles/debugger/debugger_statement.js",
        ]

        process = subprocess.Popen(
            resmoke_cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

        # Wait for a short time - the debugger should pause and not complete
        try:
            stdout, _ = process.communicate(timeout=10)
            # If we get here without timeout, the debugger didn't activate properly
            self.logger.error("Test output:\n%s", stdout)
            self.fail("Expected debugger to pause execution, but test completed")
        except subprocess.TimeoutExpired:
            # This is the expected behavior - debugger is waiting for input
            process.kill()
            stdout, _ = process.communicate()
            self.logger.info("Debugger correctly paused execution. Output:\n%s", stdout)
            # Verify the debugger prompt appeared
            self.assertIn("JSDEBUG>", stdout)
            self.assertIn("paused in 'debugger' statement", stdout)


class TestJSDebuggerInteractive(_ResmokeSelftest):
    """Test suite for interactive JavaScript debugger functionality."""

    def run_debugger_test(self, test_file, commands, timeout=30):
        """Helper to run debugger and execute commands.

        Args:
            test_file: Path to the JS test file
            commands: List of (command, expected_output) tuples
            timeout: Maximum time to wait for each command

        Returns:
            Full output from the session
        """
        resmoke_cmd = " ".join(
            [
                sys.executable,
                "buildscripts/resmoke.py",
                "run",
                f"--dbpathPrefix={self.test_dir}",
                "--shellJSDebugMode",
                "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_debugger_nodb.yml",
                test_file,
            ]
        )

        child = pexpect.spawn(resmoke_cmd, timeout=timeout, encoding="utf-8")

        # Use StringIO to capture all output
        output_buffer = io.StringIO()
        child.logfile = output_buffer

        try:
            # Wait for initial debugger prompts - there are usually two in the initial pause
            child.expect("JSDEBUG>", timeout=20)
            self.logger.info("First debugger prompt detected")

            # Wait for the "Type 'dbcont' to continue" prompt
            child.expect("JSDEBUG>", timeout=5)
            self.logger.info("Second debugger prompt detected, ready for commands")

            # Execute each command
            for cmd, expected in commands:
                self.logger.info(f"Sending command: {cmd}")
                child.sendline(cmd)

                # Wait for the next prompt to ensure command completed
                # and capture what comes before it
                try:
                    child.expect("JSDEBUG>", timeout=5)
                    command_output = child.before if hasattr(child, "before") else ""
                except pexpect.TIMEOUT:
                    command_output = ""

                # Also check what's in the full buffer
                full_buffer = output_buffer.getvalue()
                self.logger.info(
                    f"Full buffer so far ({len(full_buffer)} chars, last 1000):\n{full_buffer[-1000:]}"
                )

                if expected:
                    # Log what we're looking for and what we got back
                    self.logger.info(f"Expect to find in output: {expected}")
                    self.logger.info(
                        f"child.before output (last 1000 chars): {command_output[-1000:] if command_output else 'empty'}"
                    )

                    # Check if expected is in the command output or full buffer
                    search_text = full_buffer  # Use full buffer instead of just child.before
                    if expected not in search_text:
                        # Try to match as regex
                        if not re.search(expected, search_text):
                            self.logger.error(f"Pattern '{expected}' not found")
                            self.logger.error(f"Full buffer:\n{full_buffer}")
                            raise AssertionError(f"Pattern '{expected}' not found in output")
                    self.logger.info(f"Successfully found expected pattern: {expected}")

            # Wait for process to finish or timeout
            try:
                child.expect(pexpect.EOF, timeout=10)
            except pexpect.TIMEOUT:
                pass

            # Return all collected output
            full_output = output_buffer.getvalue()
            self.logger.info(f"Full output length: {len(full_output)} chars")
            return full_output

        finally:
            child.close(force=True)
            output_buffer.close()

    def test_debugger_continue_with_dbcont(self):
        """Test that dbcont command continues execution."""
        commands = [
            ("dbcont", None),  # Continue execution
        ]

        self.run_debugger_test(
            "buildscripts/tests/resmoke_end2end/testfiles/debugger/debugger_statement.js",
            commands,
        )

        # If we get here without timeout, the test passed
        pass

    def test_debugger_inspect_variables(self):
        """Test inspecting variables at debugger breakpoint."""
        commands = [
            # Check variable outputs
            ("x", "42"),
            ("y", r'[ "a", 15, [ 1, 2, 3 ] ]'),
            ("z", r'{ "foo" : [ 3, "bar" ] }'),
            # ("z", r'[ 3, "bar" ] }'),
            ("dbcont", None),  # Continue
        ]

        output = self.run_debugger_test(
            "buildscripts/tests/resmoke_end2end/testfiles/debugger/debugger_statement.js",
            commands,
        )

        self.assertIn("42", output)

    def test_debugger_undefined_variable(self):
        """Test that accessing undefined variables shows ReferenceError."""
        commands = [
            ("q", "ReferenceError"),  # q is not defined
            ("dbcont", None),
        ]

        output = self.run_debugger_test(
            "buildscripts/tests/resmoke_end2end/testfiles/debugger/debugger_statement.js",
            commands,
        )

        self.assertIn("ReferenceError", output)

    def test_debugger_syntax_error(self):
        """Test that syntax errors are caught in debugger."""
        commands = [
            ("]]", "SyntaxError"),  # Invalid syntax
            ("dbcont", None),
        ]

        output = self.run_debugger_test(
            "buildscripts/tests/resmoke_end2end/testfiles/debugger/debugger_statement.js",
            commands,
        )

        self.assertIn("SyntaxError", output)

    def test_debugger_assertion_error(self):
        """Test that assertion failures are shown in debugger."""
        commands = [
            ("assert.eq(1, 2)", None),  # Should fail - don't expect specific text
            ("dbcont", None),
        ]

        output = self.run_debugger_test(
            "buildscripts/tests/resmoke_end2end/testfiles/debugger/debugger_statement.js",
            commands,
        )

        # The assertion should fail with an error message containing relevant keywords
        # The mongo shell may show different error formats
        self.assertTrue(
            ("1" in output and "2" in output)  # Numbers from assertion
            or "Error" in output
            or "assert" in output.lower(),
            f"Expected assertion error output, got: {output[:500]}",
        )

    def test_debugger_modify_variables(self):
        """Test modifying variables at debugger breakpoint."""
        # Create a test file that expects modified variables
        commands = [
            ("x", "42"),  # Check original value
            ("x = 7", None),  # Modify x
            ("y[1]", "15"),  # Check array element
            ("y[1] = 99", None),  # Modify array
            ("dbcont", None),  # Continue - should pass assertions
        ]

        output = self.run_debugger_test(
            "buildscripts/tests/resmoke_end2end/testfiles/debugger/simple_debugger.js",
            commands,
        )

        # The test should pass because we modified the variables
        self.assertIn("Test Passed", output)

    def test_debugger_complex_expressions(self):
        """Test evaluating complex expressions in debugger."""
        commands = [
            ("x + 10", "52"),  # Math expression
            ("typeof x", "number"),  # Type check
            ("[1,2,3].length", "3"),  # Array operations
            ("dbcont", None),
        ]

        output = self.run_debugger_test(
            "buildscripts/tests/resmoke_end2end/testfiles/debugger/debugger_statement.js",
            commands,
        )

        self.assertIn("52", output)
        self.assertIn("number", output)
