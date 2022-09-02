"""Unit tests for todo_check.py."""

from __future__ import absolute_import

import os
import textwrap
import unittest
from tempfile import TemporaryDirectory

from typing import Iterable

import buildscripts.todo_check as under_test

# pylint: disable=invalid-name


def create_file_iterator(file_contents: str) -> Iterable[str]:
    return textwrap.dedent(file_contents.strip()).splitlines()


class TestTodo(unittest.TestCase):
    def test_line_without_a_jira_issues(self):
        content = "a line with some random text"
        todo = under_test.Todo.from_line("file", 42, f"\n{content}\n\t\n")

        self.assertEqual(todo.file_name, "file")
        self.assertEqual(todo.line_number, 42)
        self.assertEqual(todo.line, content)
        self.assertIsNone(todo.ticket)

    def test_line_with_a_server_ticket(self):
        ticket = "SERVER-12345"
        content = f"a line with {ticket} some random text"
        todo = under_test.Todo.from_line("file", 42, f"\n{content}\n\t\n")

        self.assertEqual(todo.file_name, "file")
        self.assertEqual(todo.line_number, 42)
        self.assertEqual(todo.line, content)
        self.assertEqual(todo.ticket, ticket)

    def test_line_with_a_wiredtiger_ticket(self):
        ticket = "WT-5555"
        content = f"a line with {ticket} some random text"
        todo = under_test.Todo.from_line("file", 42, f"\n{content}\n\t\n")

        self.assertEqual(todo.file_name, "file")
        self.assertEqual(todo.line_number, 42)
        self.assertEqual(todo.line, content)
        self.assertEqual(todo.ticket, ticket)


class TestTodoChecker(unittest.TestCase):
    def test_todo_checker_starts_out_empty(self):
        todos = under_test.TodoChecker()

        self.assertEqual(len(todos.found_todos.no_tickets), 0)
        self.assertEqual(len(todos.found_todos.with_tickets), 0)
        self.assertEqual(len(todos.found_todos.by_file), 0)

    def test_a_file_with_no_todos(self):
        file_contents = """
        line 0
        line 1
        this is the file contents.
        
        """
        todos = under_test.TodoChecker()

        todos.check_file("my file", create_file_iterator(file_contents))

        self.assertEqual(len(todos.found_todos.no_tickets), 0)
        self.assertEqual(len(todos.found_todos.with_tickets), 0)
        self.assertEqual(len(todos.found_todos.by_file), 0)

    def test_a_file_with_an_untagged_todo(self):
        file_contents = """
        line 0
        line 1
        /* todo this needs some updating */
        this is the file contents.
        """
        todos = under_test.TodoChecker()

        todos.check_file("my file", create_file_iterator(file_contents))

        self.assertEqual(len(todos.found_todos.no_tickets), 1)
        self.assertEqual(len(todos.found_todos.with_tickets), 0)
        self.assertEqual(len(todos.found_todos.by_file), 1)

        todo = todos.found_todos.no_tickets[0]
        self.assertEqual(todo.file_name, "my file")
        self.assertEqual(todo.line_number, 3)
        self.assertEqual(todo.ticket, None)

        self.assertEqual(todo, todos.found_todos.by_file["my file"][0])

    def test_a_file_with_a_tagged_todo(self):
        file_contents = """
        line 0
        line 1
        line 2
        /* TODO server-1234 this needs some updating */
        this is the file contents.
        """
        todos = under_test.TodoChecker()

        todos.check_file("my file", create_file_iterator(file_contents))

        self.assertEqual(len(todos.found_todos.no_tickets), 0)
        self.assertEqual(len(todos.found_todos.with_tickets), 1)
        self.assertEqual(len(todos.found_todos.by_file), 1)

        todo = todos.found_todos.with_tickets["SERVER-1234"][0]
        self.assertEqual(todo.file_name, "my file")
        self.assertEqual(todo.line_number, 4)
        self.assertEqual(todo.ticket, "SERVER-1234")

        self.assertEqual(todo, todos.found_todos.by_file["my file"][0])

    def test_report_on_ticket_will_return_true_if_ticket_is_found(self):
        file_contents = """
        line 0
        line 1
        line 2
        /* TODO server-1234 this needs some updating */
        this is the file contents.
        """
        todos = under_test.TodoChecker()
        todos.check_file("my file", create_file_iterator(file_contents))

        self.assertTrue(todos.report_on_ticket("SERVER-1234"))

    def test_report_on_ticket_will_return_false_if_ticket_is_not_found(self):
        file_contents = """
        line 0
        line 1
        line 2
        /* TODO server-1234 this needs some updating */
        this is the file contents.
        """
        todos = under_test.TodoChecker()
        todos.check_file("my file", create_file_iterator(file_contents))

        self.assertFalse(todos.report_on_ticket("SERVER-9876"))

    def test_report_all_tickets_will_return_true_if_any_ticket_is_found(self):
        file_contents = """
        line 0
        line 1
        line 2
        /* TODO server-1234 this needs some updating */
        this is the file contents.
        /* TODO server-54321 this also needs some updating */
        """
        todos = under_test.TodoChecker()
        todos.check_file("my file", create_file_iterator(file_contents))

        self.assertTrue(todos.report_on_all_tickets())

    def test_report_all_tickets_will_return_false_if_no_ticket_is_found(self):
        file_contents = """
        line 0
        line 1
        line 2
        this is the file contents.
        """
        todos = under_test.TodoChecker()
        todos.check_file("my file", create_file_iterator(file_contents))

        self.assertFalse(todos.report_on_all_tickets())


class TestValidateCommitQueue(unittest.TestCase):
    def test_revert_commits_should_not_fail(self):
        commit_message = "Reverts commit SERVER-1234"
        file_contents = """
        line 0
        line 1
        line 2
        /* TODO server-1234 this needs some updating */
        this is the file contents.
        /* TODO server-54321 this also needs some updating */
        """
        todos = under_test.TodoChecker()
        todos.check_file("my file", create_file_iterator(file_contents))

        self.assertFalse(todos.validate_commit_queue(commit_message))

    def test_todos_associated_with_commit_message_should_be_found(self):
        commit_message = "SERVER-1234 making a commit"
        file_contents = """
        line 0
        line 1
        line 2
        /* TODO server-1234 this needs some updating */
        this is the file contents.
        /* TODO server-54321 this also needs some updating */
        """
        todos = under_test.TodoChecker()
        todos.check_file("my file", create_file_iterator(file_contents))

        self.assertTrue(todos.validate_commit_queue(commit_message))

    def test_commit_messages_with_multiple_commits_search_all_of_them(self):
        commit_message = """
        Making a wiredtiger drop
        
        WT-1234
        WT-4321
        WT-9876
        """
        file_contents = """
        line 0
        line 1
        line 2
        /* TODO server-1234 this needs some updating */
        this is the file contents.
        /* TODO WT-9876 this also needs some updating */
        """
        todos = under_test.TodoChecker()
        todos.check_file("my file", create_file_iterator(file_contents))

        self.assertTrue(todos.validate_commit_queue(commit_message))

    def test_commit_messages_with_no_tickets_doesnt_cause_issues(self):
        commit_message = "A random commit"
        file_contents = """
        line 0
        line 1
        line 2
        /* TODO server-1234 this needs some updating */
        this is the file contents.
        /* TODO WT-9876 this also needs some updating */
        """
        todos = under_test.TodoChecker()
        todos.check_file("my file", create_file_iterator(file_contents))

        self.assertFalse(todos.validate_commit_queue(commit_message))


def write_file(path: str, contents: str):
    with open(path, "w") as fh:
        fh.write(contents)


class TestWalkFs(unittest.TestCase):
    def test_walk_fs_walks_the_fs(self):
        expected_files = {
            "file1.txt": "The contents of file 1",
            "file2.txt": "The contents of file 2",
        }
        with TemporaryDirectory() as tmpdir:
            write_file(os.path.join(tmpdir, "file1.txt"), expected_files["file1.txt"])
            os.makedirs(os.path.join(tmpdir, "dir0", "dir1"))
            write_file(
                os.path.join(tmpdir, "dir0", "dir1", "file2.txt"), expected_files["file2.txt"])

            seen_files = {}

            def visit_file(file_name, file_contents):
                base_name = os.path.basename(file_name)
                seen_files[base_name] = "\n".join(file_contents)

            under_test.walk_fs(tmpdir, visit_file)

            self.assertDictEqual(expected_files, seen_files)
