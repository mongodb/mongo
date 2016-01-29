#!/usr/bin/env python

from setuptools import setup
import os.path

description = open(os.path.join(os.path.dirname(__file__), 'README'), 'rt').read()

setup(name="testscenarios",
      version="0.4",
      description="Testscenarios, a pyunit extension for dependency injection",
      long_description=description,
      maintainer="Robert Collins",
      maintainer_email="robertc@robertcollins.net",
      url="https://launchpad.net/testscenarios",
      packages=['testscenarios', 'testscenarios.tests'],
      package_dir = {'':'lib'},
      classifiers = [
          'Development Status :: 6 - Mature',
          'Intended Audience :: Developers',
          'License :: OSI Approved :: BSD License',
          'License :: OSI Approved :: Apache Software License',
          'Operating System :: OS Independent',
          'Programming Language :: Python',
          'Programming Language :: Python :: 3',
          'Topic :: Software Development :: Quality Assurance',
          'Topic :: Software Development :: Testing',
          ],
      install_requires = [
        'testtools',
        ]
      )
