#!/usr/bin/env python
try:
    # If the user has setuptools / distribute installed, use it
    from setuptools import setup
except ImportError:
    # Otherwise, fall back to distutils.
    from distutils.core import setup
    extra = {}
else:
    extra = {
        'install_requires': [
            'extras',
            'testtools>=0.9.34',
        ]
    }


def _get_version_from_file(filename, start_of_line, split_marker):
    """Extract version from file, giving last matching value or None"""
    try:
        return [x for x in open(filename)
            if x.startswith(start_of_line)][-1].split(split_marker)[1].strip()
    except (IOError, IndexError):
        return None


VERSION = (
    # Assume we are in a distribution, which has PKG-INFO
    _get_version_from_file('PKG-INFO', 'Version:', ':')
    # Must be a development checkout, so use the Makefile
    or _get_version_from_file('Makefile', 'VERSION', '=')
    or "0.0")


setup(
    name='python-subunit',
    version=VERSION,
    description=('Python implementation of subunit test streaming protocol'),
    long_description=open('README').read(),
    classifiers=[
        'Intended Audience :: Developers',
        'Programming Language :: Python :: 3',
        'Programming Language :: Python',
        'Topic :: Software Development :: Testing',
    ],
    keywords='python test streaming',
    author='Robert Collins',
    author_email='subunit-dev@lists.launchpad.net',
    url='http://launchpad.net/subunit',
    packages=['subunit', 'subunit.tests'],
    package_dir={'subunit': 'python/subunit'},
    scripts = [
        'filters/subunit-1to2',
        'filters/subunit-2to1',
        'filters/subunit2gtk',
        'filters/subunit2junitxml',
        'filters/subunit2pyunit',
        'filters/subunit-filter',
        'filters/subunit-ls',
        'filters/subunit-notify',
        'filters/subunit-stats',
        'filters/subunit-tags',
        'filters/tap2subunit',
    ],
    **extra
)
