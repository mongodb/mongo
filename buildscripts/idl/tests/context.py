# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""A support module to load idl modules for testing."""

import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

import idl.ast  # noqa: F401
import idl.binder  # noqa: F401
import idl.compiler  # noqa: F401
import idl.errors  # noqa: F401
import idl.generator  # noqa: F401
import idl.parser  # noqa: F401
import idl.syntax  # noqa: F401
import idl.writer  # noqa: F401
