#!/usr/bin/env python3
# Copyright (C) 2019-present MongoDB, Inc.
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
"""Validate that the commit message is ok."""
import argparse
import collections
import logging
import re
import subprocess
import sys
from contextlib import contextmanager
from enum import IntEnum
from http import HTTPStatus
from http.client import HTTPConnection  # py3
from typing import Dict, List, Type, Tuple, Optional, Match, Any

import requests.exceptions
from jira import JIRA, Issue
from jira.exceptions import JIRAError

VALID_PATTERNS = [
    # NOTE: re.VERBOSE is for visibility / debugging. As such significant white space must be
    # escaped (e.g ' ' to \s).
    re.compile(
        r'''
        ^
        ((?P<revert>Revert)\s*[\"\']?)?                         # Revert (optional)
        ((?P<ticket>(?:EVG|SERVER|WT)-[0-9]+)\s*)               # ticket identifier
        #(?P<body>(?:(?!\(cherry\spicked\sfrom).)*)?            # To also capture the body
        ((?:(?!\(cherry\spicked\sfrom).)*)?                     # negative lookahead backport
        (?P<backport>\(cherry\spicked\sfrom.*)?                 # back port (optional)
        ''', re.MULTILINE | re.DOTALL | re.VERBOSE),
    re.compile(r'(?P<lint>^Fix lint)'),  # Allow "Fix lint" as the sole commit summary
    re.compile(r'(?P<imported>^Import (wiredtiger|tools): .*)'),  # These are public tickets
]
"""valid public patterns."""

PRIVATE_PATTERNS = [
    re.compile(
        r'''
        ^
        ((?P<revert>Revert)\s*[\"\']?)?              # Revert (optional)
        ((?P<ticket>[A-Z]+-[0-9]+)\s*)               # ticket identifier
        ''', re.MULTILINE | re.DOTALL | re.VERBOSE),
]
"""private patterns."""

INVALID_JIRA_STATUS = ('closed', )
"""List of lower cased invalid jira status strings."""

GIT_SHOW_COMMAND = ['git', 'show', '-1', '-s', '--format=%s']
"""git command line to get the last commit message."""

DEFAULT_JIRA = 'https://jira.mongodb.org'
LOGGER = logging.getLogger(__name__)

HELP_MESSAGE = 'Ask for help in the #evergreen-users slack channel.'
WARNING_MESSAGE = 'Non-blocking issues, check for error output if the merge failed.'


class Status(IntEnum):
    """Status enumeration values."""

    OK = 0
    ERROR = 1
    WARNING = 2


class Violation(collections.namedtuple('Fault', ['status', 'message', 'hint'])):
    """validation issue holder."""

    def __str__(self):
        message = [f'{self.message}']
        if self.hint:
            message += [self.hint]
        return "\n".join(message)


def get_full_message(message: List[str]) -> str:
    """
    Convert the message to a single string or get the last git commit message.

    If the input list is empty then the last git commit message is used.

    :param message: A list of the message components.
    :return: The message.
    """
    LOGGER.info('get commit message')
    if not message:
        LOGGER.info('Validating last git commit message')
        result = subprocess.check_output(GIT_SHOW_COMMAND)
        message = result.decode('utf-8')
    else:
        message = " ".join(message)
    LOGGER.info('Validating commit message \'%s\'', message)
    return message


def find_ticket(message: str) -> Dict:
    """
    Find ticket data in message.

    :param message: The commit message.
    :return: A dict of the commit message components (may be empty).
    """
    ticket = find_matching_pattern(message, VALID_PATTERNS)
    if ticket:
        ticket['public'] = True
    else:
        ticket = find_matching_pattern(message, PRIVATE_PATTERNS)
        if ticket:
            ticket['public'] = False
    return ticket


def find_matching_pattern(message: str, patterns: List[Match]) -> Dict:
    """
    Find the first matching pattern.

    :param message: The commit message.
    :param patterns: A list of regular expressions.
    :return: A dict of the commit message components (may be empty).
    """
    for valid_pattern in patterns:
        matching_pattern = valid_pattern.match(message)
        # pattern matches and there is a ticket
        if matching_pattern:
            return matching_pattern.groupdict()
    return {}


def validate_message(message: str, author: str,
                     jira: Optional[Type[JIRA]]) -> Tuple[Dict, List[Violation]]:
    """
    Validate the commit message.

    :param message: The commit message.
    :param author: The author.
    :param jira: The jira connection.
    :return: The ticket dict and violations.
    """
    LOGGER.info('validating message')
    if not message.strip():
        ticket = {}
        violations = [
            Violation(Status.ERROR, 'found empty commit message',
                      ('Hint: Have you committed your changes?\n'
                       'Hint: Is the message valid for the latest commit.'))
        ]
    else:
        ticket = find_ticket(message)
        if 'ticket' in ticket:
            violations = validate_ticket(ticket, author, jira)
        elif 'lint' in ticket or 'imported' in ticket:
            violations = []
        else:
            violations = [
                Violation(Status.ERROR, 'found a commit without a ticket',
                          'You must provide a valid public ticket.')
            ]
    return ticket, violations


def validate_ticket(ticket: Dict, author: str, jira: Optional[Type[JIRA]]) -> List[Violation]:
    """
    Validate the ticket and commit message.

    :param ticket: The extract ticket information.
    :param author: The author.
    :param jira: The jira connection.
    :return: The violations.
    """
    violations = []
    if ticket['public']:
        violations = validate_public_ticket(ticket, author, jira)
    else:
        tid = ticket['ticket']
        violations.append(
            Violation(Status.ERROR, f'private project: {tid}',
                      'You must provide a valid public ticket.'))

    return violations


def validate_status(issue: Type[Issue], ticket: Dict) -> Optional[Violation]:
    """
    Validate that the issue status is allowed.

    :param issue: The jira issue.
    :param ticket: The information extracted from the commit message.
    :return: A violation (if applicable).
    """
    backport = ticket.get('backport', False)
    revert = ticket.get('revert', False)
    if backport or revert:
        if backport:
            label = 'backport'
        else:
            label = 'revert'
        LOGGER.debug('Skipping author validation for %s', label)
        return None

    status = str(issue.fields.status).lower() if hasattr(issue.fields, "status") else None
    if status is None or status in INVALID_JIRA_STATUS:
        return Violation(Status.ERROR, f'status cannot be {status}',
                         (f'{issue} must be open.\n'
                          'Hint: You must not close the ticket before the merge completes.'))
    return None


def validate_author(issue: Type[Issue], ticket: Dict, author: str) -> Violation:
    """
    Validate that the issue author is correct.

    :param issue: The jira issue.
    :param ticket: The ticket data.
    :param author: The expected author.
    :return: A violation (if applicable).
    """
    backport = ticket.get('backport', False)
    revert = ticket.get('revert', False)
    if backport or revert:
        if backport:
            label = 'backport'
        else:
            label = 'revert'
        LOGGER.debug('Skipping author validation for %s', label)
        return None

    assignee = issue.fields.assignee if hasattr(issue.fields, 'assignee') else None
    if assignee is None:
        return Violation(Status.ERROR, 'ticket has no assignee',
                         (f'Hint: Check {issue} is the correct ticket id.\n'
                          f'Hint: Set {issue} assignee to {author}.'))
    elif assignee.name != author:
        details = (f'assignee is not author \'{assignee.name}\'('
                   f' \'{assignee.displayName}\') != \'{author}\'')
        return Violation(Status.ERROR, details, (f'Hint: Check {issue} is the correct ticket id.\n'
                                                 f'Hint: Set {issue} assignee to {author}.'))
    return None


def validate_public_ticket(ticket: Dict, author: str, jira: Type[JIRA],
                           verbose: bool = False) -> List[Violation]:
    """
    Validate the status of a public ticket.

    :param ticket: The extract ticket information.
    :param author: The author.
    :param jira: The jira connection.
    :param verbose: A flag to enable / disable verbose output.
    :return: The violations.
    """
    violations = []
    ticket_id = ticket['ticket']
    try:
        if jira is not None:
            with silence(verbose):
                issue = jira.issue(ticket_id)
            if issue:
                violation = validate_status(issue, ticket)
                if violation:
                    violations.append(violation)

                violation = validate_author(issue, ticket, author)
                if violation:
                    violations.append(violation)
    except requests.exceptions.ConnectionError:
        LOGGER.debug('%s: unexpected connection exception', exc_info=True)
        violations.append(
            Violation(Status.WARNING, f'{ticket_id}: unexpected connection exception', ''))
    except JIRAError as ex:
        LOGGER.debug('unexpected jira exception', exc_info=True)
        if ex.status_code == HTTPStatus.NOT_FOUND:
            violation = Violation(Status.ERROR, f'{ticket_id}: not found',
                                  f'Hint: Check {ticket_id} is the correct ticket id.')
        elif ex.status_code == HTTPStatus.UNAUTHORIZED:
            violation = Violation(Status.ERROR, f'{ticket_id}: private (unauthorized)',
                                  (f'Hint: Check {ticket_id} is the correct ticket id.\n'
                                   'Hint: Check jira project is public.\n'))
        else:
            violation = Violation(Status.WARNING,
                                  f'{ticket_id}: unexpected jira error {ex.status_code}', '')
        violations.append(violation)
    except ValueError:
        LOGGER.debug('unexpected exception', exc_info=True)
        violations.append(Violation(Status.WARNING, f'{ticket_id}: unexpected exception', ''))
    except Exception as ex:  # pylint: disable=broad-except
        LOGGER.debug('unexpected exception', exc_info=True)
        violations.append(Violation(Status.WARNING, f'{ticket_id}: unexpected exception {ex}', ''))

    return violations


def handle_violations(ticket: Dict, message: str, violations: List[Violation],
                      warning_as_errors: bool) -> Type[Status]:
    """
    Handle any validation issues found.

    :param ticket: The extract ticket information.
    :param message: The commit message.
    :param violations: The validation violations.
    :param warning_as_errors: If True then treat all violations as errors.
    :return: The Status.ERROR if no errors or warning_as_errors.
    """
    LOGGER.info('handle validation issues')
    if warning_as_errors:
        errors = violations
        warnings = []
    else:
        errors = [validation for validation in violations if validation.status == Status.ERROR]
        warnings = [validation for validation in violations if validation.status == Status.WARNING]

    if errors:
        print(f"Message: \'{message.strip()}\'\nTicket: \'{ticket.get('ticket', '')}\'\n")
        for error in errors:
            LOGGER.error("%s\n", str(error))

    if warnings:
        print(WARNING_MESSAGE)
        for warning in warnings:
            LOGGER.error("%s\n", str(warning))

    return Status.ERROR if errors else Status.OK


def jira_client(jira_server: str, verbose: bool = False) -> Optional[Type[JIRA]]:
    """
    Connect to jira.

    Create a connection to jira_server and validate that SERVER-1 is accessible.
    :param jira_server: The jira server endpoint to connect and validate.
    :param verbose: A flag controlling th verbosity of the checks. The requests and jira package
    are very verbose by default.
    :return: The jira client instance if all went well.
    """
    try:
        # requests and JIRA can be very verbose.
        LOGGER.info('connecting to %s', jira_server)
        with silence(verbose):
            jira = JIRA(jira_server, logging=verbose)
            # check the status of a known / existing ticket. A JIRAError with a status code of
            # 404 Not Found maybe returned or a 401 unauthorized.
            jira.issue("SERVER-1")
            return jira
    except (requests.exceptions.ConnectionError, JIRAError, ValueError) as ex:
        # These are recoverable / ignorable exceptions. We print exception the full stack trace
        # when debugging / verbose output is requested.
        # ConnectionErrors relate to networking.
        # JIRAError, ValueError refer to invalid / unexpected responses.
        class_name = _get_class_name(ex)
        if isinstance(ex, requests.exceptions.ConnectionError):
            details = f'{class_name}: unable to connect to {jira_server}'
        elif isinstance(ex, JIRAError):
            details = f'{class_name}: unable to access {jira_server}, status: {ex.status_code}'
        elif isinstance(ex, ValueError):
            details = f'{class_name}: communication error with {jira_server}'
        LOGGER.debug(details, exc_info=True)
    except Exception as ex:  # pylint: disable=broad-except
        # recoverable / ignorable exceptions but unknown so print trace.
        LOGGER.debug('%s unknown error: %s', _get_class_name(ex), jira_server, exc_info=True)
    return None


def configure_logging(level: int, formatter: str = '%(levelname)s: %(message)s'):
    """
    Configure logging.

    :param level: The log level verbosity. 0 logs warnings and above. 1 enabled info and above, all
    other values are DEBUG and above.
    :param formatter: The log formatter.
    """
    # level 0 is the default level (warning or greater).
    logging.basicConfig(format=formatter)
    root_logger = logging.getLogger()
    debuglevel = 0
    if level == 0:
        level = logging.WARNING
    elif level == 1:
        level = logging.INFO
    elif level >= 2:
        debuglevel = 1
        level = logging.DEBUG

    root_logger.setLevel(level)
    HTTPConnection.debuglevel = debuglevel


def _get_class_name(obj: Any) -> str:
    """Get the class name without package."""
    return type(obj).__name__


@contextmanager
def silence(disable: bool = False):
    """
    Silence logging within this scope.

    :param disable: A flag to programmatically enable / disable the silence functionality. Useful
    for debugging.
    """
    logger = logging.getLogger()
    old = logger.disabled
    try:
        if not disable:
            logger.disabled = True
        yield
    finally:
        logger.disabled = old


def parse_args(argv: List[str]) -> Type[argparse.ArgumentParser]:
    """
    Parse the command line args.

    :param argv: The command line arguments.
    :return: The parsed arguments.
    """
    parser = argparse.ArgumentParser(
        usage="Validate the commit message. "
        "It validates the latest message when no arguments are provided.")
    parser.add_argument(
        "-a",
        '--author',
        dest="author",
        nargs='?',
        const=1,
        type=str,
        help="Your jira username of the author. This value must match the JIRA assignee.",
        required=True,
    )
    parser.add_argument(
        "-j",
        dest="jira_server",
        nargs='?',
        const=1,
        type=str,
        help="The jira server location. Defaults to '" + DEFAULT_JIRA + "'",
        default=DEFAULT_JIRA,
    )
    parser.add_argument(
        "-W",
        action="store_true",
        dest="warning_as_errors",
        help="treat warnings as errors.",
        default=False,
    )
    parser.add_argument("-v", "--verbosity", action="count", default=0,
                        help="increase output verbosity")
    parser.add_argument(
        "message",
        metavar="commit message",
        nargs="*",
        help="The commit message to validate",
    )
    args = parser.parse_args(argv)
    return args


def main(argv: List = None) -> Type[Status]:
    """
    Execute main function to validate commit messages.

    :param argv: The command line arguments.
    :return: Status.OK if the commit message validation passed other wise Status.ERROR.
    """

    args = parse_args(argv)
    configure_logging(level=args.verbosity)
    jira = jira_client(args.jira_server)

    message = get_full_message(args.message)
    ticket, violations = validate_message(message, args.author, jira)

    return handle_violations(ticket, message, violations, args.warning_as_errors)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
