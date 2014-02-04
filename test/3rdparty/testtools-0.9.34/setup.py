#!/usr/bin/env python
"""Distutils installer for testtools."""

from setuptools import setup
from distutils.command.build_py import build_py
import email
import os
import sys

import testtools
cmd_class = {}
if getattr(testtools, 'TestCommand', None) is not None:
    cmd_class['test'] = testtools.TestCommand


class testtools_build_py(build_py):
    def build_module(self, module, module_file, package):
        if sys.version_info >= (3,) and module == '_compat2x':
            return
        return build_py.build_module(self, module, module_file, package)
cmd_class['build_py'] = testtools_build_py


def get_version_from_pkg_info():
    """Get the version from PKG-INFO file if we can."""
    pkg_info_path = os.path.join(os.path.dirname(__file__), 'PKG-INFO')
    try:
        pkg_info_file = open(pkg_info_path, 'r')
    except (IOError, OSError):
        return None
    try:
        pkg_info = email.message_from_file(pkg_info_file)
    except email.MessageError:
        return None
    return pkg_info.get('Version', None)


def get_version():
    """Return the version of testtools that we are building."""
    version = '.'.join(
        str(component) for component in testtools.__version__[0:3])
    phase = testtools.__version__[3]
    if phase == 'final':
        return version
    pkg_info_version = get_version_from_pkg_info()
    if pkg_info_version:
        return pkg_info_version
    # Apparently if we just say "snapshot" then distribute won't accept it
    # as satisfying versioned dependencies. This is a problem for the
    # daily build version.
    return "snapshot-%s" % (version,)


def get_long_description():
    manual_path = os.path.join(
        os.path.dirname(__file__), 'doc/overview.rst')
    return open(manual_path).read()


setup(name='testtools',
      author='Jonathan M. Lange',
      author_email='jml+testtools@mumak.net',
      url='https://github.com/testing-cabal/testtools',
      description=('Extensions to the Python standard library unit testing '
                   'framework'),
      long_description=get_long_description(),
      version=get_version(),
      classifiers=["License :: OSI Approved :: MIT License",
        "Programming Language :: Python :: 3",
        ],
      packages=[
        'testtools',
        'testtools.matchers',
        'testtools.testresult',
        'testtools.tests',
        'testtools.tests.matchers',
        ],
      cmdclass=cmd_class,
      zip_safe=False,
      install_requires=[
        'extras',
        # 'mimeparse' has not been uploaded by the maintainer with Python3 compat
        # but someone kindly uploaded a fixed version as 'python-mimeparse'.
        'python-mimeparse',
        ],
      )
