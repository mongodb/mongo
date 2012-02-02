#!/usr/bin/env python
"""Distutils installer for testtools."""

from distutils.core import setup
import email
import os

import testtools


def get_revno():
    import bzrlib.errors
    import bzrlib.workingtree
    try:
        t = bzrlib.workingtree.WorkingTree.open_containing(__file__)[0]
    except (bzrlib.errors.NotBranchError, bzrlib.errors.NoWorkingTree):
        return None
    else:
        return t.branch.revno()


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
    revno = get_revno()
    if revno is None:
        return "snapshot"
    if phase == 'alpha':
        # No idea what the next version will be
        return 'next-r%s' % revno
    else:
        # Preserve the version number but give it a revno prefix
        return version + '-r%s' % revno


def get_long_description():
    manual_path = os.path.join(
        os.path.dirname(__file__), 'doc/overview.rst')
    return open(manual_path).read()


setup(name='testtools',
      author='Jonathan M. Lange',
      author_email='jml+testtools@mumak.net',
      url='https://launchpad.net/testtools',
      description=('Extensions to the Python standard library unit testing '
                   'framework'),
      long_description=get_long_description(),
      version=get_version(),
      classifiers=["License :: OSI Approved :: MIT License"],
      packages=['testtools', 'testtools.testresult', 'testtools.tests'],
      cmdclass={'test': testtools.TestCommand})
