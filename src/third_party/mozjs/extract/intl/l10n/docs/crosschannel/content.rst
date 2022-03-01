=====================
Cross-channel Content
=====================

When creating the actual content, there's a number of questions to answer.

#. Where to take content from?
#. Which content to take?
#. Where to put the content?
#. What to put into each file?

Content Sources
---------------

The content of each revision in ``gecko-strings`` corresponds to a given
revision in each original repository. For example, we have

+------------------+--------------+
| Repository       | Revision     |
+==================+==============+
| mozilla-central  | 4c92802939c1 |
+------------------+--------------+
| mozilla-beta     | ace4081e8200 |
+------------------+--------------+
| mozilla-release  | 2cf08fbb92b2 |
+------------------+--------------+
| mozilla-esr68    | 2cf9e0c91d51 |
+------------------+--------------+
| comm-central     | 3f3fc2c0d804 |
+------------------+--------------+
| comm-beta        | f95a6f4408a3 |
+------------------+--------------+
| comm-release     | dc2694f035fa |
+------------------+--------------+
| comm-esr68       | d05d4d87d25c |
+------------------+--------------+

At this point, there's no content that's shared between ``mozilla-*`` and
``comm-*``. Thus we can just convert one repository and its branches at a time.

Covered Content
---------------

Which content is included in ``gecko-strings`` is
controlled by the project configurations of each product, on each branch.
At this point, those are

* :file:`browser/locales/l10n.toml`, :file:`mobile/android/locales/l10n.toml`
  in ``mozilla-central``, and
* :file:`mail/locales/l10n.toml`, :file:`calendar/locales/l10n.toml`, and
  :file:`suite/locales/l10n.toml` in ``comm-central``.

Created Content Structure
-------------------------

The created content is laid out in the directory in the same structure as
the files in ``l10n-central``. The localizable files end up like this:

.. code-block:: 

   browser/
     browser/
       browser.ftl
   chrome/
     browser.properties
   toolkit/
     toolkit/
       about/aboutAbout.ftl

This matches the file locations in ``mozilla-central`` with the
:file:`locales/en-US` part dropped.

The project configuration files are also converted and added to the
created file structure. As they're commonly in the :file:`locales` folder
which we strip, they're added to the dedicated :file:`_configs` folder.

.. code-block:: bash

   $ ls _configs
   browser.toml   devtools-client.toml  mail.toml            suite.toml
   calendar.toml  devtools-shared.toml  mobile-android.toml  toolkit.toml


L10n File Contents
------------------

Let's assume we have a file to localize in several revisions with different
content.

== ======= ==== =======
ID central beta release
== ======= ==== =======
a  one     one  one
b          two  two
c  three
d  four    old  old
== ======= ==== =======

The algorithm then creates content, taking localizable values from the left-most
branch, where *central* overrides *beta*, and *beta* overrides *release*. This
creates content as follows:

== =======
ID content
== =======
a  one
b  two
c  three
d  four
== =======

If a file doesn't exist in one of the revisions, that revision is dropped
from the content generation for this particular file.

.. note::

   The example of the forth string here highlights the impact that changing
   an existing string has. We ship one translation of *four* to central,
   beta, and release. That's only a good idea if it doesn't matter which of the
   two versions of the English copy got translated.

Project configurations
----------------------

The TOML files for project configuration are processed, but not unified
across branches at this point.

.. note::

   The content of the ``-central`` branch determines what's localized
   from ``gecko-strings``. Thus that TOML file needs to include all
   directories across all branches for now. Removing entries requires
   that the content is obsolete on all branches in cross-channel.
