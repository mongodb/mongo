"""Unit tests for the evergreen_task_timeout script."""
import logging
import unittest
from http import HTTPStatus
from http.client import HTTPConnection

import requests.exceptions
from mock import MagicMock, patch, PropertyMock
from jira import JIRAError

from buildscripts.validate_commit_message import Status, jira_client, \
    configure_logging, validate_public_ticket, get_full_message, GIT_SHOW_COMMAND, \
    validate_message, handle_violations, Violation, validate_ticket, find_ticket, main
# pylint: disable=missing-docstring,no-self-use
from buildscripts.validate_commit_message import find_matching_pattern, VALID_PATTERNS

AUTHOR = 'jim'

INVALID_MESSAGES = [
    [""],  # You must provide a message
    ["RevertEVG-1"],  # revert and ticket must be formatted
    ["revert EVG-1"],  # revert must be capitalized
    ["This is not a valid message"],  # message must be valid
    ["Fix lint plus extras is not a valid message"],  # Fix lint is strict
]

NS = "buildscripts.validate_commit_message"


def ns(relative_name):  # pylint: disable=invalid-name
    """Return a full name from a name relative to the tested module's name space."""
    return NS + "." + relative_name


class GetFullMessageTest(unittest.TestCase):
    def test_no_message(self):
        with patch(ns('subprocess')) as mock_subprocess:
            expected = "output"
            mock_subprocess.check_output.return_value = bytearray(expected, "utf-8")
            self.assertEqual(expected, get_full_message([]))
            mock_subprocess.check_output.assert_called_once_with(GIT_SHOW_COMMAND)

    def test_empty_message(self):
        with patch(ns('subprocess')) as mock_subprocess:
            # blank / empty string is false
            self.assertFalse(get_full_message([""]))
            mock_subprocess.check_output.assert_not_called()

    def test_message(self):
        with patch(ns('subprocess')) as mock_subprocess:
            expected = "this is a test"
            self.assertEqual(expected, get_full_message(expected.split()))
            mock_subprocess.check_output.assert_not_called()


class FindTicketTest(unittest.TestCase):
    def test_last_git_commit_success(self):
        messages = [
            _make_expected("Fix lint", lint='Fix lint', public=True),
            _make_expected(
                'SERVER-44612 '
                'this is the message\n\n'
                '(cherry picked from commit beef)', revert=None, ticket='SERVER-44612',
                backport='(cherry picked from commit beef)', public=True),
            _make_expected('SERVER-45074 this is a test (cherry picked from', revert=None,
                           ticket='SERVER-45074', backport='(cherry picked from', public=True),
            _make_expected('SERVER-45074 this is a test (cherry picked from commit beef)',
                           revert=None, ticket='SERVER-45074',
                           backport='(cherry picked from commit beef)', public=True),
            _make_expected('SERVER-20', revert=None, ticket='SERVER-20', backport=None,
                           public=True),
            _make_expected('WT-300', revert=None, ticket='WT-300', backport=None, public=True),
            _make_expected('SERVER-44338', revert=None, ticket='SERVER-44338', backport=None,
                           public=True),
            _make_expected('Revert EVG-5', revert='Revert', ticket='EVG-5', backport=None,
                           public=True),
            _make_expected('Revert SERVER-60', revert='Revert', ticket='SERVER-60', backport=None,
                           public=True),
            _make_expected('Revert WT-700', revert='Revert', ticket='WT-700', backport=None,
                           public=True),
            _make_expected('Revert \'SERVER-8000', revert='Revert', ticket='SERVER-8000',
                           backport=None, public=True),
            _make_expected('Revert \'SERVER-8000\'', revert='Revert', ticket='SERVER-8000',
                           backport=None, public=True),
            _make_expected('Revert "SERVER-90000"', revert='Revert', ticket='SERVER-90000',
                           backport=None, public=True),
            _make_expected('Revert "SERVER-90000"', revert='Revert', ticket='SERVER-90000',
                           backport=None, public=True),
            _make_expected(
                'Import wiredtiger: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69 from '
                'branch mongodb-4.4',
                imported='Import wiredtiger: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69'
                ' from branch mongodb-4.4', public=True),
            _make_expected(
                'Import tools: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69 from '
                'branch mongodb-4.4',
                imported='Import tools: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69'
                ' from branch mongodb-4.4', public=True),
            _make_expected('PRIVATE-1 this is a test', ticket='PRIVATE-1', public=False,
                           revert=None),
            _make_expected('Revert PRIVATE-1 this is a test', ticket='PRIVATE-1', public=False,
                           revert='Revert'),
            _make_expected('this is a test'),
        ]
        self.assertTrue(all(find_ticket(message) == expected for message, expected in messages))


class FindMatchingPatternTest(unittest.TestCase):
    def test_last_git_commit_success(self):
        messages = [
            _make_expected("Fix lint", lint='Fix lint'),
            _make_expected(
                'SERVER-44612 '
                'this is the message\n\n'
                '(cherry picked from commit beef)', revert=None, ticket='SERVER-44612',
                backport='(cherry picked from commit beef)'),
            _make_expected('SERVER-45074 this is a test (cherry picked from', revert=None,
                           ticket='SERVER-45074', backport='(cherry picked from'),
            _make_expected('SERVER-45074 this is a test (cherry picked from commit beef)',
                           revert=None, ticket='SERVER-45074',
                           backport='(cherry picked from commit beef)'),
            _make_expected('SERVER-20', revert=None, ticket='SERVER-20', backport=None),
            _make_expected('WT-300', revert=None, ticket='WT-300', backport=None),
            _make_expected('SERVER-44338', revert=None, ticket='SERVER-44338', backport=None),
            _make_expected('Revert EVG-5', revert='Revert', ticket='EVG-5', backport=None),
            _make_expected('Revert SERVER-60', revert='Revert', ticket='SERVER-60', backport=None),
            _make_expected('Revert WT-700', revert='Revert', ticket='WT-700', backport=None),
            _make_expected('Revert \'SERVER-8000', revert='Revert', ticket='SERVER-8000',
                           backport=None),
            _make_expected('Revert \'SERVER-8000\'', revert='Revert', ticket='SERVER-8000',
                           backport=None),
            _make_expected('Revert "SERVER-90000"', revert='Revert', ticket='SERVER-90000',
                           backport=None),
            _make_expected('Revert "SERVER-90000"', revert='Revert', ticket='SERVER-90000',
                           backport=None),
            _make_expected(
                'Import wiredtiger: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69 from '
                'branch mongodb-4.4',
                imported='Import wiredtiger: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69'
                ' from branch mongodb-4.4'),
            _make_expected(
                'Import tools: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69 from '
                'branch mongodb-4.4',
                imported='Import tools: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69'
                ' from branch mongodb-4.4'),
        ]
        self.assertTrue(
            all(
                find_matching_pattern(message, VALID_PATTERNS) == expected
                for message, expected in messages))


def _mock_issue(author, status='open', display="Jim O'Leary", assignee=True):
    if assignee:
        mock_assignee = MagicMock(name='assignee', displayName=display)
        type(mock_assignee).name = PropertyMock(return_value=author)
        mock_fields = MagicMock(name='fields', status=status, assignee=mock_assignee)
    else:
        mock_fields = MagicMock(name='fields', status=status, spec=['status'])
    mock_issue = MagicMock(name='issue', fields=mock_fields)
    return mock_issue


class ValidateMessageTest(unittest.TestCase):
    def _test_empty_message(self, message=''):
        mock_jira = MagicMock(name='jira')
        ticket, validations = validate_message(message, AUTHOR, mock_jira)
        self.assertFalse(ticket)
        self.assertEqual(1, len(validations))
        self.assertEqual(Status.ERROR, validations[0].status)
        self.assertIn('empty commit', validations[0].message)
        self.assertIn('Have you committed your changes', validations[0].hint)
        self.assertIn('Is the message valid for the latest commit', validations[0].hint)

    def test_empty_message(self):
        self._test_empty_message('')

    def test_blank_message(self):
        self._test_empty_message(' ')

    def test_validate(self):
        mock_jira = MagicMock(name='jira')
        message = 'SERVER-45730 validate commit messages'
        ticket = {'revert': None, 'ticket': 'SERVER-45730', 'backport': None, 'public': True}
        validations = []
        with patch(ns('find_ticket'), return_value=ticket) as mock_find_ticket, \
                patch(ns('validate_ticket'), return_value=validations) as mock_validate_ticket:
            actual_ticket, actual_validations = validate_message(message, AUTHOR, mock_jira)
            self.assertEqual(actual_ticket, ticket)
            self.assertEqual(actual_validations, validations)
            mock_find_ticket.assert_called_once_with(message)
            mock_validate_ticket.assert_called_once_with(ticket, AUTHOR, mock_jira)

    def test_lint(self):
        mock_jira = MagicMock(name='jira')
        message = 'Fix lint'
        ticket = dict(lint=message, public=True)
        validations = []
        with patch(ns('validate_ticket'), return_value=validations) as mock_validate_ticket:
            actual_ticket, actual_validations = validate_message(message, AUTHOR, mock_jira)
            self.assertEqual(actual_ticket, ticket)
            self.assertEqual(actual_validations, validations)
            mock_validate_ticket.assert_not_called()

    def test_import_wiredtiger(self):
        mock_jira = MagicMock(name='jira')
        message = 'Import wiredtiger: 58baf804dd6e5a72c4e122cfb696e2d06a9fc888 from'
        ticket = dict(imported=message, public=True)
        validations = []
        with patch(ns('validate_ticket'), return_value=validations) as mock_validate_ticket:
            actual_ticket, actual_validations = validate_message(message, AUTHOR, mock_jira)
            self.assertEqual(actual_ticket, ticket)
            self.assertEqual(actual_validations, validations)
            mock_validate_ticket.assert_not_called()

    def test_import_tools(self):
        mock_jira = MagicMock(name='jira')
        message = 'Import tools: 58baf804dd6e5a72c4e122cfb696e2d06a9fc888 from'
        ticket = dict(imported=message, public=True)
        validations = []
        with patch(ns('validate_ticket'), return_value=validations) as mock_validate_ticket:
            actual_ticket, actual_validations = validate_message(message, AUTHOR, mock_jira)
            self.assertEqual(actual_ticket, ticket)
            self.assertEqual(actual_validations, validations)
            mock_validate_ticket.assert_not_called()

    def test_invalid(self):
        mock_jira = MagicMock(name='jira')
        message = 'message'
        ticket = dict()
        validations = [
            Violation(status=Status.ERROR, message='found a commit without a ticket',
                      hint='You must provide a valid public ticket.')
        ]
        with patch(ns('validate_ticket'), return_value=validations) as mock_validate_ticket:
            actual_ticket, actual_validations = validate_message(message, AUTHOR, mock_jira)
            self.assertEqual(actual_ticket, ticket)
            self.assertEqual(actual_validations, validations)
            mock_validate_ticket.assert_not_called()


class ValidateTicketTest(unittest.TestCase):
    def test_public_ticket(self):
        mock_jira = MagicMock(name='jira')
        ticket = dict(public=True)
        expected = []
        with patch(ns('validate_public_ticket'),
                   return_value=expected) as mock_validate_public_ticket:
            self.assertEqual(expected, validate_ticket(ticket, AUTHOR, mock_jira))
            mock_validate_public_ticket.assert_called_once_with(ticket, AUTHOR, mock_jira)

    def test_private_ticket(self):
        mock_jira = MagicMock(name='jira')
        ticket = dict(public=False, ticket='PRIVATE-1')
        expected = []
        with patch(ns('validate_public_ticket'), return_value=expected):
            validations = validate_ticket(ticket, AUTHOR, mock_jira)
            self.assertEqual(1, len(validations))
            self.assertEqual(Status.ERROR, validations[0].status)
            self.assertIn('private project', validations[0].message)
            self.assertIn('You must provide a valid public ticket', validations[0].hint)


class ValidatePublicTicketTest(unittest.TestCase):
    def test_closed(self):
        mock_jira = MagicMock(name='jira')
        mock_jira.issue.return_value = _mock_issue(AUTHOR, status='closed')
        ticket = dict(ticket='SERVER-45074')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertEqual(len(faults), 1)
        self.assertEqual(faults[0].status, Status.ERROR)
        self.assertIn('status cannot be', faults[0].message)
        self.assertIn('You must not close the ticket before the merge completes.', faults[0].hint)

    def test_closed_revert(self):
        mock_jira = MagicMock(name='jira')
        mock_jira.issue.return_value = _mock_issue(AUTHOR, status='closed')
        ticket = dict(ticket='SERVER-45074', revert='Revert')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertFalse(faults)

    def test_closed_backport(self):
        mock_jira = MagicMock(name='jira')
        mock_jira.issue.return_value = _mock_issue(AUTHOR, status='closed')
        ticket = dict(ticket='SERVER-45074', backport='cherry picked from')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertFalse(faults)

    def test_closed_revert_author(self):
        mock_jira = MagicMock(name='jira')
        mock_jira.issue.return_value = _mock_issue('not' + AUTHOR, status='closed')
        ticket = dict(ticket='SERVER-45074', revert='Revert')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertFalse(faults)

    def test_closed_backport_author(self):
        mock_jira = MagicMock(name='jira')
        mock_jira.issue.return_value = _mock_issue('not' + AUTHOR, status='closed')
        ticket = dict(ticket='SERVER-45074', backport='cherry picked from')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertFalse(faults)

    def test_revert_none(self):
        mock_jira = MagicMock(name='jira')
        mock_jira.issue.return_value = _mock_issue('not' + AUTHOR, assignee=False)
        ticket = dict(ticket='SERVER-45074', revert='Revert')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertFalse(faults)

    def test_assignee_none(self):
        mock_jira = MagicMock(name='jira', )
        mock_jira.issue.return_value = _mock_issue(AUTHOR, assignee=False)
        ticket = dict(ticket='SERVER-45074')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertEqual(len(faults), 1)
        self.assertEqual(faults[0].status, Status.ERROR)
        self.assertIn('ticket has no assignee', faults[0].message)
        self.assertIn('assignee to', faults[0].hint)
        self.assertIn('is the correct ticket', faults[0].hint)

    def test_different_author(self):
        mock_jira = MagicMock(name='jira')
        mock_jira.issue.return_value = _mock_issue('not ' + AUTHOR)
        ticket = dict(ticket='SERVER-45074')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertEqual(len(faults), 1)
        self.assertEqual(faults[0].status, Status.ERROR)
        self.assertIn('assignee is not author', faults[0].message)
        self.assertIn('assignee to', faults[0].hint)

    def test_different_author_revert(self):
        mock_jira = MagicMock(name='jira')
        mock_jira.issue.return_value = _mock_issue('not ' + AUTHOR)
        ticket = dict(ticket='SERVER-45074', revert='Revert')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertFalse(faults)

    def test_different_author_backport(self):
        mock_jira = MagicMock(name='jira')
        mock_jira.issue.return_value = _mock_issue('not ' + AUTHOR)
        ticket = dict(ticket='SERVER-45074', backport='cherry picked from')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertFalse(faults)

    def test_multiple(self):
        mock_jira = MagicMock(name='jira', )
        mock_jira.issue.return_value = _mock_issue('not ' + AUTHOR, status='closed')
        ticket = dict(ticket='SERVER-45074')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertEqual(len(faults), 2)
        self.assertTrue(all([fault for fault in faults if fault.status == Status.ERROR]))
        self.assertTrue(any(['assignee is not author' in fault.message for fault in faults]))
        self.assertTrue(any(['status cannot be' in fault.message for fault in faults]))

    def test_backport_not_author(self):
        mock_jira = MagicMock(name='jira')
        mock_jira.issue.return_value = _mock_issue('not ' + AUTHOR)
        ticket = dict(ticket='SERVER-45074', backport='(cherry picled from X)')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertFalse(faults)

    def test_backport_author(self):
        mock_jira = MagicMock(name='jira')
        mock_jira.issue.return_value = _mock_issue(AUTHOR)
        ticket = dict(ticket='SERVER-45074', backport='(cherry picled from X)')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertFalse(faults)

    def test_exception(self):
        mock_jira = MagicMock(name='jira')
        mock_jira.issue.side_effect = Exception('Boom!')
        ticket = dict(ticket='SERVER-45074')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertEqual(len(faults), 1)
        self.assertEqual(faults[0].status, Status.WARNING)
        self.assertIn('unexpected exception', faults[0].message)
        self.assertEqual('', faults[0].hint)

    def test_connection_exception(self):
        mock_jira = MagicMock(name='jira')
        mock_jira.issue.side_effect = requests.exceptions.ConnectionError('boom')
        ticket = dict(ticket='SERVER-45074')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertEqual(1, len(faults))
        self.assertTrue(faults[0].status == Status.WARNING)
        self.assertIn('unexpected connection exception', faults[0].message)
        self.assertEqual('', faults[0].hint)

    def test_not_found(self):
        mock_jira = MagicMock(name='jira')
        mock_jira.issue.side_effect = JIRAError(status_code=HTTPStatus.NOT_FOUND)
        ticket = dict(ticket='SERVER-45074')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertEqual(1, len(faults))
        self.assertTrue(faults[0].status == Status.ERROR)
        self.assertIn('not found', faults[0].message)
        self.assertIn('Check SERVER-45074 is the correct ticket id', faults[0].hint)

    def test_permission(self):
        mock_jira = MagicMock(name='jira')
        mock_jira.issue.side_effect = JIRAError(status_code=HTTPStatus.UNAUTHORIZED)
        ticket = dict(ticket='SERVER-45074')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertEqual(1, len(faults))
        self.assertTrue(faults[0].status == Status.ERROR)
        self.assertIn('private', faults[0].message)
        self.assertIn('is the correct ticket id.', faults[0].hint)
        self.assertIn('Check jira project is public.', faults[0].hint)
        self.assertIn('Check SERVER-45074 is the correct ticket id', faults[0].hint)

    def test_other_jira_exception(self):
        mock_jira = MagicMock(name='jira')
        mock_jira.issue.side_effect = JIRAError()
        ticket = dict(ticket='SERVER-45074')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertEqual(1, len(faults))
        self.assertTrue(faults[0].status == Status.WARNING)
        self.assertIn('unexpected jira error', faults[0].message)
        self.assertEqual('', faults[0].hint)

    def test_value_error(self):
        mock_jira = MagicMock(name='jira')
        mock_jira.issue.side_effect = ValueError()
        ticket = dict(ticket='SERVER-45074')
        faults = validate_public_ticket(ticket, AUTHOR, mock_jira)
        self.assertEqual(1, len(faults))
        self.assertTrue(faults[0].status == Status.WARNING)
        self.assertIn('unexpected exception', faults[0].message)
        self.assertEqual('', faults[0].hint)


class HandleValidationTest(unittest.TestCase):
    def test_empty(self):
        ticket = {}
        message = 'this is a test'
        validations = []
        warnings_as_errors = False
        self.assertEqual(Status.OK,
                         handle_violations(ticket, message, validations, warnings_as_errors))

    def test_error(self):
        ticket = {}
        message = 'this is a test'
        validations = [Violation(Status.ERROR, 'private project: PRIVATE-1', 'HINT')]
        warnings_as_errors = False
        self.assertEqual(Status.ERROR,
                         handle_violations(ticket, message, validations, warnings_as_errors))

    def _test_warnings(self, validations=None, warnings_as_errors=False):
        ticket = {}
        message = 'this is a test'
        if validations is None:
            validations = [
                Violation(Status.WARNING, 'found a commit without a ticket', 'HINT'),
                Violation(Status.WARNING, 'another warning', 'HINT')
            ]
        self.assertEqual(Status.OK if not warnings_as_errors else Status.ERROR,
                         handle_violations(ticket, message, validations, warnings_as_errors))

    def test_single_warning(self):
        self._test_warnings([Violation(Status.WARNING, 'found a commit without a ticket', 'HINT')])

    def test_multiple_warnings(self):
        self._test_warnings()

    def test_warning_as_error(self):
        self._test_warnings(warnings_as_errors=True)

    def test_warnings_and_errors(self):
        ticket = {}
        message = 'this is a test'
        validations = [
            Violation(Status.ERROR, 'private project: PRIVATE-1', 'HINT'),
            Violation(Status.WARNING, 'found a commit without a ticket', 'HINT')
        ]
        warnings_as_errors = False
        self.assertEqual(Status.ERROR,
                         handle_violations(ticket, message, validations, warnings_as_errors))


def _make_expected(message, **kwargs):
    return message, dict(**kwargs)


class JiraClientTest(unittest.TestCase):
    def _test_exception(self, exception):
        with patch(ns("JIRA"), side_effect=exception("boom")):
            self.assertIsNone(jira_client('server', verbose=False))

    def test_connection_exceptions(self):
        """test connection exceptions."""
        self._test_exception(requests.exceptions.ConnectionError)

    def test_jira_exception(self):
        """test jira exceptions."""
        self._test_exception(JIRAError)

    def test_value_exception(self):
        """test ValueError exceptions."""
        self._test_exception(ValueError)

    def test_exception(self):
        """test Exception exceptions."""
        self._test_exception(Exception)

    def test_issue(self):
        mock_jira = MagicMock()
        # mock_jira.issue.return_value = 'issue'
        with patch(ns("JIRA"), return_value=mock_jira) as mock_ctor:
            jira = jira_client('server')
            mock_ctor.assert_called_once_with('server', logging=False)
            mock_jira.issue.assert_called_once_with('SERVER-1')
            self.assertEqual(mock_jira, jira)


class ConfigureLoggingTest(unittest.TestCase):
    def _test_configure_formatter(self, formatter=None):
        mock_root_logger = MagicMock(name="root_logger")
        with patch.object(logging, 'basicConfig') as mock_basic_config, \
                patch.object(logging, 'getLogger') as mock_get_logger:
            mock_get_logger.return_value = mock_root_logger
            if formatter is None:
                configure_logging(0)
            else:
                configure_logging(0, formatter=formatter)

            if formatter is None:
                expected = '%(levelname)s: %(message)s'
            else:
                expected = formatter

            mock_basic_config.assert_called_once_with(format=expected)
            mock_root_logger.setLevel.assert_called_once_with(logging.WARNING)

    def test_configure_default_formatter(self):
        self._test_configure_formatter()

    def test_configure_formatter(self):
        self._test_configure_formatter(formatter='format')

    def _test_configure_level(self, level, expected, debuglevel=0):
        old_debuglevel = HTTPConnection.debuglevel
        try:
            mock_root_logger = MagicMock(name="root_logger")
            with patch.object(logging, 'basicConfig'), \
                    patch.object(logging, 'getLogger') as mock_get_logger:
                mock_get_logger.return_value = mock_root_logger
                configure_logging(level)

                mock_root_logger.setLevel.assert_called_once_with(expected)
                self.assertEqual(HTTPConnection.debuglevel, debuglevel)
        finally:
            HTTPConnection.debuglevel = old_debuglevel

    def test_configure_level_0(self):
        self._test_configure_level(0, logging.WARNING)

    def test_configure_level_1(self):
        self._test_configure_level(1, logging.INFO)

    def test_configure_level_2(self):
        self._test_configure_level(2, logging.DEBUG, debuglevel=1)

    def test_configure_level_3(self):
        self._test_configure_level(3, logging.DEBUG, debuglevel=1)


class MainTest(unittest.TestCase):
    def _test_helper(self, args=None, violations=None, expected=Status.OK):
        if violations is None:
            violations = []
        if args is None:
            args = []
        args = ['SERVER-45074 this is a test', '-a', AUTHOR] + args
        with patch(ns('jira_client')), patch(ns('validate_public_ticket'), return_value=violations):
            self.assertEqual(expected, main(args))

    def test_no_violations(self):
        self._test_helper()

    def test_warning(self):
        violations = [Violation(Status.WARNING, 'just a warning', 'HINT')]
        self._test_helper(violations=violations)

    def test_warning_as_error(self):
        violations = [Violation(Status.WARNING, 'just a warning', 'HINT')]
        args = ['-W']
        self._test_helper(args=args, violations=violations, expected=Status.ERROR)

    def test_error(self):
        violations = [Violation(Status.ERROR, 'just an error', 'HINT')]
        self._test_helper(violations=violations, expected=Status.ERROR)

    def test_no_author(self):
        args = ['SERVER-45074 this is a test']
        with self.assertRaises(SystemExit) as cm:
            main(args)
        exception = cm.exception
        self.assertEqual(exception.code, 2)

    def test_author(self):
        args = ['SERVER-45074 this is a test', '-a', AUTHOR]
        with patch(ns('JIRA'), side_effect=requests.exceptions.ConnectionError):
            self.assertEqual(main(args), 0)
