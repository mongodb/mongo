"""Generator fixture that imports a helper and reads data from tool runfiles."""

import pathlib
import sys

from testdata.helper import message

pathlib.Path(sys.argv[1]).write_text(message(), encoding="utf-8")
