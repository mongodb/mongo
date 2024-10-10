#!/usr/bin/env python3
"""Check for TODOs in the source code."""

import os
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from typing import Callable, Dict, Iterable, List, NamedTuple, Optional

import click

from evergreen import RetryingEvergreenApi

EVG_CONFIG_FILE = "./.evergreen.yml"
BASE_SEARCH_DIR = "."
IGNORED_PATHS = [".git"]
ISSUE_RE = re.compile("(BUILD|SERVER|WT|PM|TOOLS|TIG|PERF|BF)-[0-9]+")


class Todo(NamedTuple):
    """
    A TODO comment found the in the code.

    file_name: Name of file comment was found in.
    line_number: Line number comment was found in.
    line: Content of line comment was found in.
    ticket: Jira ticket associated with comment.
    """

    file_name: str
    line_number: int
    line: str
    ticket: Optional[str] = None

    @classmethod
    def from_line(cls, file_name: str, line_number: int, line: str) -> "Todo":
        """
        Create a found todo from the given line of code.

        :param file_name: File name comment was found in.
        :param line_number: Line number comment was found in.
        :param line: Content of line.
        :return: Todo representation of found comment.
        """
        issue_key = cls.get_issue_key_from_line(line)
        return cls(file_name, line_number, line.strip(), issue_key)

    @staticmethod
    def get_issue_key_from_line(line: str) -> Optional[str]:
        """
        Check if the given line appears to reference a jira ticket.

        :param line: Content of line.
        :return: Jira ticket if one was found.
        """
        match = ISSUE_RE.search(line.upper())
        if match:
            return match.group(0)
        return None


@dataclass
class FoundTodos:
    """
    Collection of TODO comments found in the code base.

    no_tickets: TODO comments found without any Jira ticket references.
    with_tickets: Dictionary of Jira tickets references with a list of references.
    by_file: All the references found mapped by the files they were found in.
    """

    no_tickets: List[Todo]
    with_tickets: Dict[str, List[Todo]]
    by_file: Dict[str, List[Todo]]


class TodoChecker:
    """A tool to find and track TODO references."""

    def __init__(self) -> None:
        """Initialize a new TODO checker."""
        self.found_todos = FoundTodos(
            no_tickets=[], with_tickets=defaultdict(list), by_file=defaultdict(list)
        )

    def check_file(self, file_name: str, file_contents: Iterable[str]) -> None:
        """
        Check the given file for TODO references.

        Any TODOs will be added to `found_todos`.

        :param file_name: Name of file being checked.
        :param file_contents: Iterable of the file contents.
        """
        for i, line in enumerate(file_contents):
            if "todo" in line.lower():
                todo = Todo.from_line(file_name, i + 1, line)
                if todo.ticket is not None:
                    self.found_todos.with_tickets[todo.ticket].append(todo)
                else:
                    self.found_todos.no_tickets.append(todo)
                self.found_todos.by_file[file_name].append(todo)

    def check_all_files(self, base_dir: str) -> None:
        """
        Check all files under the base directory for TODO references.

        :param base_dir: Base directory to start searching.
        """
        walk_fs(base_dir, self.check_file)

    @staticmethod
    def print_todo_list(todo_list: List[Todo]) -> None:
        """Display all the TODOs in the given list."""
        last_file = None
        for todo in todo_list:
            if last_file != todo.file_name:
                print(f"{todo.file_name}")
            print(f"\t{todo.line_number}: {todo.line}")

    def report_on_ticket(self, ticket: str) -> bool:
        """
        Report on any TODOs found referencing the given ticket.

        Any found references will be printed to stdout.

        :param ticket: Jira ticket to search for.
        :return: True if any TODOs were found.
        """
        todo_list = self.found_todos.with_tickets.get(ticket)
        if todo_list:
            print(f"{ticket}")
            self.print_todo_list(todo_list)
            return True
        return False

    def report_on_all_tickets(self) -> bool:
        """
        Report on all TODOs found that reference a Jira ticket.

        Any found references will be printed to stdout.

        :return: True if any TODOs were found.
        """
        if not self.found_todos.with_tickets:
            return False

        for ticket in self.found_todos.with_tickets.keys():
            self.report_on_ticket(ticket)

        return True

    def validate_commit_queue(self, commit_message: str) -> bool:
        """
        Check that the given commit message does not reference TODO comments.

        :param commit_message: Commit message to check.
        :return: True if any TODOs were found.
        """
        print("*" * 80)
        print("Checking for TODOs associated with Jira key in commit message.")
        if "revert" in commit_message.lower():
            print("Skipping checks since it looks like this is a revert.")
            # Reverts are a special case and we shouldn't fail them.
            return False

        found_any = False
        ticket = Todo.get_issue_key_from_line(commit_message)
        while ticket:
            found_any = self.report_on_ticket(ticket) or found_any
            rest_index = commit_message.find(ticket)
            commit_message = commit_message[rest_index + len(ticket) :]
            ticket = Todo.get_issue_key_from_line(commit_message)

        print(f"Checking complete - todos found: {found_any}")
        print("*" * 80)
        return found_any


def walk_fs(root: str, action: Callable[[str, Iterable[str]], None]) -> None:
    """
    Walk the file system and perform the given action on each file.

    :param root: Base to start walking the filesystem.
    :param action: Action to perform on each file.
    """
    for base, _, files in os.walk(root):
        for file_name in files:
            try:
                file_path = os.path.join(base, file_name)
                if any(ignore in file_path for ignore in IGNORED_PATHS):
                    continue

                with open(file_path) as search_file:
                    action(file_path, search_file)
            except UnicodeDecodeError:
                # If we try to read any non-text files.
                continue


def get_summary_for_patch(version_id: str) -> str:
    """
    Get the description provided for the given patch build.

    :param version_id: Version ID of the patch build to query.
    :return: Description provided for the patch build.
    """
    evg_api = RetryingEvergreenApi.get_api(config_file=EVG_CONFIG_FILE)
    return evg_api.version_by_id(version_id).message


@click.command()
@click.option("--ticket", help="Only report on TODOs associated with given Jira ticket.")
@click.option("--base-dir", default=BASE_SEARCH_DIR, help="Base directory to search in.")
@click.option(
    "--commit-message", help="For commit-queue execution only, ensure no TODOs for this commit"
)
@click.option(
    "--patch-build",
    type=str,
    help="For patch build execution only, check for any TODOs from patch description",
)
def main(
    ticket: Optional[str], base_dir: str, commit_message: Optional[str], patch_build: Optional[str]
):
    """
    Search for and report on TODO comments in the code base.

    Based on the arguments given, there are two types of searches you can perform:

    \b
    * By default, search for all TODO comments that reference any Jira ticket.
    * Search for references to a specific Jira ticket with the `--ticket` option.

    \b
    Examples
    --------

        \b
        Search all TODO comments with Jira references:
        ```
        > python buildscripts/todo_check.py
        SERVER-12345
        ./src/mongo/db/file.cpp
            140: // TODO: SERVER-12345: Need to fix this.
            183: // TODO: SERVER-12345: Need to fix this as well.
        SERVER-54321
        ./src/mongo/db/other_file.h
            728: // TODO: SERVER-54321 an update is needed here
        ```

        \b
        Search for any TODO references to a given ticket.
        ```
        > python buildscripts/todo_check.py --ticket SERVER-1234
        SERVER-1234
        ./src/mongo/db/file.cpp
            140: // TODO: SERVER-1234: Need to fix this.
        ```

    \b
    Exit Code
    ---------
        In any mode, if any TODO comments are found a non-zero exit code will be returned.
    \f
    :param ticket: Only report on TODOs associated with this jira ticket.
    :param base_dir: Search files in this base directory.
    :param commit_message: Commit message if running in the commit-queue.
    :param patch_build: Version ID of patch build to check.

    """
    if commit_message and ticket is not None:
        raise click.UsageError("--ticket cannot be used in commit queue.")

    if patch_build:
        if ticket is not None:
            raise click.UsageError("--ticket cannot be used in patch builds.")
        commit_message = get_summary_for_patch(patch_build)

    todo_checker = TodoChecker()
    todo_checker.check_all_files(base_dir)

    if commit_message:
        found_todos = todo_checker.validate_commit_queue(commit_message)
    elif ticket:
        found_todos = todo_checker.report_on_ticket(ticket)
    else:
        found_todos = todo_checker.report_on_all_tickets()

    if found_todos:
        print(
            "TODOs that reference a Jira ticket associated with the current commit should not "
            "remain in the code after the commit is merged. A TODO referencing a ticket that has "
            "been closed and that solved the TODO's purpose can be confusing."
        )
        print(
            "To fix this error resolve any TODOs that reference the Jira ticket this commit is"
            "associated with."
        )
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()  # pylint: disable=no-value-for-parameter
