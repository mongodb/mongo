Contributing to the MongoDB project
===================================

Pull requests are always welcome, and the MongoDB dev team appreciates any help the community can
give to help make MongoDB better.

For any particular improvement you want to make, you can begin a discussion on the
`MongoDB Developers Forum`_.  This is the best place discuss your proposed improvement (and its
implementation) with the core development team.

.. _MongoDB Developers Forum: https://groups.google.com/forum/?fromgroups#!forum/mongodb-dev


Getting Started
---------------

- Create a `MongoDB JIRA account`_.
- Create a `Github account`_.
- Fork the repository on Github at https://github.com/mongodb/mongo.

.. _MongoDB JIRA account: https://jira.mongodb.org/secure/Signup!default.jspa
.. _Github account: https://github.com/signup/free


JIRA Tickets
------------

All commits to the MongoDB repository must reference an issue in the `SERVER project`_ of the
MongoDB JIRA.  Before creating any new tickets, please search the existing backlog for any open
tickets that represent your change request.  If there is not one, then you should create a new
ticket.

For bugs, please clearly describe the issue you are resolving, including the platforms on which
the issue is present and clear steps to reproduce.

For improvements or feature requests, be sure to explain the goal or use case and the approach
your solution will take.

.. _SERVER project: https://jira.mongodb.org/browse/SERVER


Style Guide
-----------

All commits to the MongoDB repository must follow the `kernel development rules`_.

In particular, all code must follow the MongoDB `kernel code style guidelines`_.  For anything
not covered in this document you should default to the `Google CPP Style Guide`_ and the
`Google JavaScript Style Guide`_.

Your commit message should also be prefaced with the relevant JIRA ticket, e.g. "SERVER-XXX Fixed
a bug in aggregation".

.. _kernel development rules: http://dochub.mongodb.org/core/kernelcodedevelopmentrules
.. _Kernel Code Style guidelines: http://dochub.mongodb.org/core/kernelcodestyle
.. _Google CPP Style Guide: http://google-styleguide.googlecode.com/svn/trunk/cppguide.xml
.. _Google JavaScript Style Guide: http://google-styleguide.googlecode.com/svn/trunk/javascriptguide.xml


Testing
-------

Every non-trivial change to the code base should be accompanied by a relevant addition to or
modification of the test suite.  If you don't believe this is necessary, please add an explanation
in the JIRA ticket why no such changes are either needed or possible.

All changes must also pass the full test suite (including your test additions/changes) on your
local machine before you open a pull request.


Contributor Agreement
---------------------

A patch will only be considered for merging into the upstream codebase after you have signed the
`contributor agreement`_.

.. _contributor agreement: http://www.mongodb.com/contributor