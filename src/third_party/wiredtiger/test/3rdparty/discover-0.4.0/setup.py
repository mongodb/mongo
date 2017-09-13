#!/usr/bin/env python
# setup.py
# Install script for discover.py
# Copyright (C) 2009-2010 Michael Foord
# E-mail: michael AT voidspace DOT org DOT uk

# This software is licensed under the terms of the BSD license.
# http://www.voidspace.org.uk/python/license.shtml

import sys
from distutils.core import setup
from discover import __version__ as VERSION


NAME = 'discover'
MODULES = ('discover',)
DESCRIPTION = 'Test discovery for unittest. Backported from Python 2.7 for Python 2.4+'
URL = 'http://pypi.python.org/pypi/discover/'
CLASSIFIERS = [
    'Development Status :: 4 - Beta',
    'Environment :: Console',
    'Intended Audience :: Developers',
    'License :: OSI Approved :: BSD License',
    'Programming Language :: Python',
    'Programming Language :: Python :: 2.4',
    'Programming Language :: Python :: 2.5',
    'Programming Language :: Python :: 2.6',
    'Programming Language :: Python :: 2.7',
    'Programming Language :: Python :: 3',
    'Programming Language :: Python :: 3.0',
    'Programming Language :: Python :: 3.1',
    'Programming Language :: Python :: 3.2',
    'Operating System :: OS Independent',
    'Topic :: Software Development :: Libraries',
    'Topic :: Software Development :: Libraries :: Python Modules',
    'Topic :: Software Development :: Testing',
]
AUTHOR = 'Michael Foord'
AUTHOR_EMAIL = 'michael@voidspace.org.uk'
KEYWORDS = "unittest, testing, tests".split(', ')
LONG_DESCRIPTION = open('README.txt').read()


params = dict(
    name=NAME,
    version=VERSION,
    description=DESCRIPTION,
    long_description=LONG_DESCRIPTION,
    author=AUTHOR,
    author_email=AUTHOR_EMAIL,
    url=URL,
    py_modules=MODULES,
    classifiers=CLASSIFIERS,
    keywords=KEYWORDS
)


try:
    from setuptools import setup
except ImportError:
    from distutils.core import setup
else:
    params.update(dict(
        entry_points = {
            'console_scripts': [
                'discover = discover:main',
                ],
            },
    ))
    params['test_suite'] = 'discover.collector'

setup(**params)


