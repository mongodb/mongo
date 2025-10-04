#!/usr/bin/env python3
#  subunit: extensions to python unittest to get test results from subprocesses.
#  Copyright (C) 2009  Robert Collins <robertc@robertcollins.net>
#
#  Licensed under either the Apache License, Version 2.0 or the BSD 3-clause
#  license at the users choice. A copy of both licenses are available in the
#  project source as Apache-2.0 and BSD. You may not use this file except in
#  compliance with one of these two licences.
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under these licenses is distributed on an "AS IS" BASIS, WITHOUT
#  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
#  license you chose for the specific language governing permissions and
#  limitations under that license.
#

### The GTK progress bar __init__ function is derived from the pygtk tutorial:
# The PyGTK Tutorial is Copyright (C) 2001-2005 John Finlay.
#
# The GTK Tutorial is Copyright (C) 1997 Ian Main.
#
# Copyright (C) 1998-1999 Tony Gale.
#
# Permission is granted to make and distribute verbatim copies of this manual
# provided the copyright notice and this permission notice are preserved on all
# copies.
#
# Permission is granted to copy and distribute modified versions of this
# document under the conditions for verbatim copying, provided that this
# copyright notice is included exactly as in the original, and that the entire
# resulting derived work is distributed under the terms of a permission notice
# identical to this one.
#
# Permission is granted to copy and distribute translations of this document
# into another language, under the above conditions for modified versions.
#
# If you are intending to incorporate this document into a published work,
# please contact the maintainer, and we will make an effort to ensure that you
# have the most up to date information available.
#
# There is no guarantee that this document lives up to its intended purpose.
# This is simply provided as a free resource. As such, the authors and
# maintainers of the information provided within can not make any guarantee
# that the information is even accurate.

"""Display a subunit stream in a gtk progress window."""

import sys
import threading
import unittest

import gi

gi.require_version('Gtk', '3.0')
from gi.repository import GObject, Gtk    # noqa: E402
from testtools import StreamToExtendedDecorator  # noqa: E402

from subunit import (PROGRESS_POP, PROGRESS_PUSH, PROGRESS_SET,  # noqa: E402
                     ByteStreamToStreamResult)
from subunit.progress_model import ProgressModel  # noqa: E402


class GTKTestResult(unittest.TestResult):

    def __init__(self):
        super(GTKTestResult, self).__init__()
        # Instance variables (in addition to TestResult)
        self.window = None
        self.run_label = None
        self.ok_label = None
        self.not_ok_label = None
        self.total_tests = None

        self.window = Gtk.Window(Gtk.WindowType.TOPLEVEL)
        self.window.set_resizable(True)

        self.window.connect("destroy", Gtk.main_quit)
        self.window.set_title("Tests...")
        self.window.set_border_width(0)

        vbox = Gtk.VBox(False, 5)
        vbox.set_border_width(10)
        self.window.add(vbox)
        vbox.show()

        # Create a centering alignment object
        align = Gtk.Alignment.new(0.5, 0.5, 0, 0)
        vbox.pack_start(align, False, False, 5)
        align.show()

        # Create the ProgressBar
        self.pbar = Gtk.ProgressBar()
        align.add(self.pbar)
        self.pbar.set_text("Running")
        self.pbar.show()
        self.progress_model = ProgressModel()

        separator = Gtk.HSeparator()
        vbox.pack_start(separator, False, False, 0)
        separator.show()

        # rows, columns, homogeneous
        table = Gtk.Table(2, 3, False)
        vbox.pack_start(table, False, True, 0)
        table.show()
        # Show summary details about the run. Could use an expander.
        label = Gtk.Label(label="Run:")
        table.attach(label, 0, 1, 1, 2, Gtk.AttachOptions.EXPAND | Gtk.AttachOptions.FILL,
            Gtk.AttachOptions.EXPAND | Gtk.AttachOptions.FILL, 5, 5)
        label.show()
        self.run_label = Gtk.Label(label="N/A")
        table.attach(self.run_label, 1, 2, 1, 2, Gtk.AttachOptions.EXPAND | Gtk.AttachOptions.FILL,
            Gtk.AttachOptions.EXPAND | Gtk.AttachOptions.FILL, 5, 5)
        self.run_label.show()

        label = Gtk.Label(label="OK:")
        table.attach(label, 0, 1, 2, 3, Gtk.AttachOptions.EXPAND | Gtk.AttachOptions.FILL,
            Gtk.AttachOptions.EXPAND | Gtk.AttachOptions.FILL, 5, 5)
        label.show()
        self.ok_label = Gtk.Label(label="N/A")
        table.attach(self.ok_label, 1, 2, 2, 3, Gtk.AttachOptions.EXPAND | Gtk.AttachOptions.FILL,
            Gtk.AttachOptions.EXPAND | Gtk.AttachOptions.FILL, 5, 5)
        self.ok_label.show()

        label = Gtk.Label(label="Not OK:")
        table.attach(label, 0, 1, 3, 4, Gtk.AttachOptions.EXPAND | Gtk.AttachOptions.FILL,
            Gtk.AttachOptions.EXPAND | Gtk.AttachOptions.FILL, 5, 5)
        label.show()
        self.not_ok_label = Gtk.Label(label="N/A")
        table.attach(self.not_ok_label, 1, 2, 3, 4, Gtk.AttachOptions.EXPAND | Gtk.AttachOptions.FILL,
            Gtk.AttachOptions.EXPAND | Gtk.AttachOptions.FILL, 5, 5)
        self.not_ok_label.show()

        self.window.show()
        # For the demo.
        self.window.set_keep_above(True)
        self.window.present()

    def stopTest(self, test):
        super(GTKTestResult, self).stopTest(test)
        GObject.idle_add(self._stopTest)

    def _stopTest(self):
        self.progress_model.advance()
        if self.progress_model.width() == 0:
            self.pbar.pulse()
        else:
            pos = self.progress_model.pos()
            width = self.progress_model.width()
            percentage = (pos / float(width))
            self.pbar.set_fraction(percentage)

    def stopTestRun(self):
        try:
            super(GTKTestResult, self).stopTestRun()
        except AttributeError:
            pass
        GObject.idle_add(self.pbar.set_text, 'Finished')

    def addError(self, test, err):
        super(GTKTestResult, self).addError(test, err)
        GObject.idle_add(self.update_counts)

    def addFailure(self, test, err):
        super(GTKTestResult, self).addFailure(test, err)
        GObject.idle_add(self.update_counts)

    def addSuccess(self, test):
        super(GTKTestResult, self).addSuccess(test)
        GObject.idle_add(self.update_counts)

    def addSkip(self, test, reason):
        super(GTKTestResult, self).addSkip(test, reason)
        GObject.idle_add(self.update_counts)

    def addExpectedFailure(self, test, err):
        super(GTKTestResult, self).addExpectedFailure(test, err)
        GObject.idle_add(self.update_counts)

    def addUnexpectedSuccess(self, test):
        super(GTKTestResult, self).addUnexpectedSuccess(test)
        GObject.idle_add(self.update_counts)

    def progress(self, offset, whence):
        if whence == PROGRESS_PUSH:
            self.progress_model.push()
        elif whence == PROGRESS_POP:
            self.progress_model.pop()
        elif whence == PROGRESS_SET:
            self.total_tests = offset
            self.progress_model.set_width(offset)
        else:
            self.total_tests += offset
            self.progress_model.adjust_width(offset)

    def time(self, a_datetime):
        # We don't try to estimate completion yet.
        pass

    def update_counts(self):
        self.run_label.set_text(str(self.testsRun))
        bad = len(self.failures + self.errors)
        self.ok_label.set_text(str(self.testsRun - bad))
        self.not_ok_label.set_text(str(bad))


def main():
    GObject.threads_init()
    result = StreamToExtendedDecorator(GTKTestResult())
    test = ByteStreamToStreamResult(sys.stdin, non_subunit_name='stdout')
    # Get setup
    while Gtk.events_pending():
        Gtk.main_iteration()

    # Start IO
    def run_and_finish():
        test.run(result)
        result.stopTestRun()
    t = threading.Thread(target=run_and_finish)
    t.daemon = True
    result.startTestRun()
    t.start()
    Gtk.main()
    if result.decorated.wasSuccessful():
        exit_code = 0
    else:
        exit_code = 1
    sys.exit(exit_code)


if __name__ == '__main__':
    main()
