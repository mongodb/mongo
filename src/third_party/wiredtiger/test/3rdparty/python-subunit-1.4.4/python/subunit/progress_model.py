#
#  subunit: extensions to Python unittest to get test results from subprocesses.
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

"""Support for dealing with progress state."""

class ProgressModel(object):
    """A model of progress indicators as subunit defines it.
    
    Instances of this class represent a single logical operation that is
    progressing. The operation may have many steps, and some of those steps may
    supply their own progress information. ProgressModel uses a nested concept
    where the overall state can be pushed, creating new starting state, and
    later pushed to return to the prior state. Many user interfaces will want
    to display an overall summary though, and accordingly the pos() and width()
    methods return overall summary information rather than information on the
    current subtask.

    The default state is 0/0 - indicating that the overall progress is unknown.
    Anytime the denominator of pos/width is 0, rendering of a ProgressModel
    should should take this into consideration.

    :ivar: _tasks. This private attribute stores the subtasks. Each is a tuple:
        pos, width, overall_numerator, overall_denominator. The overall fields
        store the calculated overall numerator and denominator for the state
        that was pushed.
    """

    def __init__(self):
        """Create a ProgressModel.
        
        The new model has no progress data at all - it will claim a summary
        width of zero and position of 0.
        """
        self._tasks = []
        self.push()

    def adjust_width(self, offset):
        """Adjust the with of the current subtask."""
        self._tasks[-1][1] += offset

    def advance(self):
        """Advance the current subtask."""
        self._tasks[-1][0] += 1

    def pop(self):
        """Pop a subtask off the ProgressModel.

        See push for a description of how push and pop work.
        """
        self._tasks.pop()

    def pos(self):
        """Return how far through the operation has progressed."""
        if not self._tasks:
            return 0
        task = self._tasks[-1]
        if len(self._tasks) > 1:
            # scale up the overall pos by the current task or preserve it if
            # no current width is known.
            offset = task[2] * (task[1] or 1)
        else:
            offset = 0
        return offset + task[0]

    def push(self):
        """Push a new subtask.

        After pushing a new subtask, the overall progress hasn't changed. Calls
        to adjust_width, advance, set_width will only after the progress within
        the range that calling 'advance' would have before - the subtask
        represents progressing one step in the earlier task.

        Call pop() to restore the progress model to the state before push was
        called.
        """
        self._tasks.append([0, 0, self.pos(), self.width()])

    def set_width(self, width):
        """Set the width of the current subtask."""
        self._tasks[-1][1] = width

    def width(self):
        """Return the total width of the operation."""
        if not self._tasks:
            return 0
        task = self._tasks[-1]
        if len(self._tasks) > 1:
            # scale up the overall width by the current task or preserve it if
            # no current width is known.
            return task[3] * (task[1] or 1)
        else:
            return task[1]

