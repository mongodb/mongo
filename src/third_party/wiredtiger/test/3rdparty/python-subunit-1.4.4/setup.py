#!/usr/bin/env python3

import os.path

from setuptools import setup


def _get_version_from_file(filename, start_of_line, split_marker):
    """Extract version from file, giving last matching value or None"""
    try:
        return [
            x for x in open(filename) if x.startswith(start_of_line)
        ][-1].split(split_marker)[1].strip()
    except (IOError, IndexError):
        return None


VERSION = (
    # Assume we are in a distribution, which has PKG-INFO
    _get_version_from_file('PKG-INFO', 'Version:', ':')
    # Must be a development checkout, so use the Makefile
    or _get_version_from_file('Makefile', 'VERSION', '=')
    or "0.0"
)


relpath = os.path.dirname(__file__)
if relpath:
    os.chdir(relpath)

setup(
    name='python-subunit',
    version=VERSION,
    description=('Python implementation of subunit test streaming protocol'),
    long_description=open('README.rst').read(),
    classifiers=[
        'Intended Audience :: Developers',
        'Operating System :: OS Independent',
        'Programming Language :: Python',
        'Programming Language :: Python :: 3',
        'Programming Language :: Python :: 3.7',
        'Programming Language :: Python :: 3.8',
        'Programming Language :: Python :: 3.9',
        'Programming Language :: Python :: 3.10',
        'Programming Language :: Python :: 3.11',
        'Programming Language :: Python :: 3.12',
        'Topic :: Software Development :: Testing',
    ],
    keywords='python test streaming',
    author='Robert Collins',
    author_email='subunit-dev@lists.launchpad.net',
    url='http://launchpad.net/subunit',
    license='Apache-2.0 or BSD',
    project_urls={
        "Bug Tracker": "https://bugs.launchpad.net/subunit",
        "Source Code": "https://github.com/testing-cabal/subunit/",
    },
    packages=['subunit', 'subunit.tests', 'subunit.filter_scripts'],

    package_dir={'subunit': 'python/subunit'},
    python_requires=">=3.7",
    install_requires=[
        'iso8601',
        'testtools>=0.9.34',
    ],
    entry_points={
        'console_scripts': [
            'subunit-1to2=subunit.filter_scripts.subunit_1to2:main',
            'subunit-2to1=subunit.filter_scripts.subunit_2to1:main',
            'subunit-filter=subunit.filter_scripts.subunit_filter:main',
            'subunit-ls=subunit.filter_scripts.subunit_ls:main',
            'subunit-notify=subunit.filter_scripts.subunit_notify:main',
            'subunit-output=subunit.filter_scripts.subunit_output:main',
            'subunit-stats=subunit.filter_scripts.subunit_stats:main',
            'subunit-tags=subunit.filter_scripts.subunit_tags:main',
            'subunit2csv=subunit.filter_scripts.subunit2csv:main',
            'subunit2disk=subunit.filter_scripts.subunit2disk:main',
            'subunit2gtk=subunit.filter_scripts.subunit2gtk:main',
            'subunit2junitxml=subunit.filter_scripts.subunit2junitxml:main',
            'subunit2pyunit=subunit.filter_scripts.subunit2pyunit:main',
            'tap2subunit=subunit.filter_scripts.tap2subunit:main',
        ]
    },
    tests_require=[
        'fixtures',
        'hypothesis',
        'testscenarios',
    ],
    extras_require={
        'docs': ['docutils'],
        'test': ['fixtures', 'testscenarios', 'hypothesis'],
    },
)
