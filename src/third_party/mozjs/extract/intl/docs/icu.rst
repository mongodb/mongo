###
ICU
###

Introduction
============

Internationalization (i18n, “i” then 18 letters then “n”) is the process of handling data with respect to a particular locale:

-  The number 5 representing five US dollars might be formatted as

   -  “$5.00” in American English,
   -  “US$5.00” in Canadian English, or
   -  “5,00 $US” in French.

-  A list of people’s names in a phone book would sort

   -  in English alphabetically; but
   -  in German, where “ä”/“ö”/“ü” are often interchangeable with “ae”/“oe”/“ue”, alphabetically but with vowels with umlauts treated as their two-vowel counterparts.

-  The currency whose code is “CHF” might be formatted as

   -  “Swiss Franc” in English, but
   -  “franc suisse” in French.

-  The Unix time 1590803313070 might format as the time string

   -  “9:48:33 PM Eastern Daylight Time” in American English, but
   -  “21:48:33 Nordamerikanische Ostküsten-Sommerzeit” in German.

i18n encompasses far more than this, but you get the basic idea.

Internationalization in SpiderMonkey and Gecko
==============================================

SpiderMonkey implements extensive i18n capabilities through the `ECMAScript Internationalization API <https://tc39.es/ecma402/>`__ and the global ``Intl`` object. Gecko requires i18n capabilities to implement text shaping, sort operations in some contexts, and various other features.

SpiderMonkey and Gecko use `ICU <http://site.icu-project.org/>`__, Internationalization Components for Unicode, to implement many low-level i18n operations. (Line breaking, implemented instead in ``intl/lwbrk``, is a notable exception.) Gecko and SpiderMonkey also use ICU’s implementations of certain i18n-*adjacent* operations (for example, Unicode normalization).

ICU date/time formatting functionality requires extensive knowledge of time zone names and when zone transitions occur. The IANA ``tzdata`` database supplies this information.

A final note of caution: ICU carefully depends upon an exact Unicode version. Other parts of SpiderMonkey and Gecko have separate dependencies on an exact Unicode version. Updates to ICU and related components *must* be synchronized with those updates so that the entirety of SpiderMonkey, and the entirety of Gecko including SpiderMonkey within it, advance to new Unicode versions in lockstep. [#lockstep]_

.. [#lockstep]
   The steps involved in updating Gecko-in-general’s Unicode version, and updating SpiderMonkey’s code dependent on Unicode version, are `documented on WikiMO <https://wiki.mozilla.org/I18n:Updating_Unicode_version>`__.

Building SpiderMonkey or Gecko with ICU
=======================================

SpiderMonkey and Gecko can be built using either a periodically-updated copy of ICU in ``intl/icu/source`` (using time zone data in ``intl/tzdata/source``), or using a system-provided ICU library (dependent on its own ``tzdata`` information). Pass ``--with-system-icu`` when configuring to use system ICU. (Using system ICU will disable some ``Intl`` functionality, such as historically accurate time zone calculations, that can’t be readily supported without a precisely-controlled ICU.) ICU version requirements advance fairly quickly as Gecko depends on features and bug fixes in newer ICU releases. You’ll get a build error if you try to use an unsupported ICU.

SpiderMonkey’s ``Intl`` API may be built or disabled by configuring ``--with-intl-api`` (the default) or ``--without-intl-api``. SpiderMonkey built without the ``Intl`` API doesn’t require ICU. However, if you build without the ``Intl`` API, some non-``Intl`` JavaScript functionality will not exist (``String.prototype.normalize``) or won’t fully work (for example, ``String.prototype.toLocale{Lower,Upper}Case`` will not respect a provided locale, and the various ``toLocaleString`` functions have best-effort behavior).

Using ICU functionality in SpiderMonkey and Gecko
=================================================

ICU headers are considered system headers by the Gecko build system, so they must be listed in ``config/system-headers.mozbuild``. Code that wishes to use ICU functionality may use ``#include "unicode/unorm.h"`` or similar to do so.

Gecko and SpiderMonkey code may use ICU’s stable C API (ICU4C). These functions are stable and shouldn’t change as ICU updates occur. (ICU4C’s ``enum`` initializers are not always stable: while initializer values are stable, new initializers are sometimes added, perhaps behind ``#ifdef U_HIDE_DRAFT_API``. This may be necessary for exhaustive ``switch``\ es to add ``#ifdef``\ s around some ``case``\ s.)

Gecko and SpiderMonkey are strongly discouraged from using ICU’s C++ API (unfortunately including all smart pointer classes), because the C++ API doesn’t provide ICU4C’s compatibility guarantees. Rarely, we tolerate C++ API use when no stable option exists. But the API has to “look” reasonably stable, and we usually want to start a discussion with upstream about adding a stable API to eventually use. Use symbols from ``namespace icu`` to access ICU C++ functionality. *Talk to the current imported-ICU owner (presently Jeff Walden) before you start doing any of this!*

SpiderMonkey and Gecko’s imported ICU
=====================================

Build system
------------

The system for building ICU lives in ``config/external/icu`` and ``intl/icu/icu_sources_data.py``. We generate a Mozilla-compatible build system rather than using ICU’s build system. The build system is shared by SpiderMonkey and Gecko both.

ICU includes functionality we never use, so we don’t naively compile all of it. We extract the list of files to compile from ``intl/icu/source/{common,i18n}/Makefile.in`` and then apply a manually-maintained list of unused files (stored in ``intl/icu_sources_data.py``) when we update ICU.

Locale and time zone data
-------------------------

ICU contains a considerable amount of raw locale data: formatting characteristics for each locale, strings for things like currencies and languages for each locale, localized time zone specifiers, and so on. This data lives in human-readable files in ``intl/icu/source/data``. Time zone data in ``intl/tzdata/source`` is stored in partially-compiled formats (some of them only partly human-readable).

However, a normal Gecko build never uses these files! Instead, both ICU and ``tzdata`` data are precompiled into a large, endian-specific ``icudtNNE.dat`` (``NN`` = ICU version, ``E`` = endianness) file. [#why-icudt-not-rebuilt-every-time]_ That file is added to ``config/external/icu/data/`` and is checked into the Mozilla tree, to be directly incorporated into Gecko/SpiderMonkey builds. For size reasons, only the little-endian version is checked into the tree. It is converted into a big-endian version when necessary during the build.

ICU’s locale data covers *all* ICU internationalization features, including ones we never need. We trim locale data to size with a ``intl/icu/data_filter.json`` `data filter <https://github.com/unicode-org/icu/blob/master/docs/userguide/icu_data/buildtool.md>`__ when compiling ``icudtNNE.dat``. Removing *too much* data won’t *necessarily* break the build, so it’s important that we have automated tests for the locale data we actually use in order to detect mistakes.

.. [#why-icudt-not-rebuilt-every-time]
   ``icudtNNE.dat`` isn’t compiled during a SpiderMonkey/Gecko build because it would require ICU command-line tools. And it’s a pain to either compile and run them during the build, or to require them as build dependencies.

Local patching of ICU
---------------------

We generally don’t patch our copy of ICU except for compelling need. When we do patch, we usually only apply reasonably small patches that have been reviewed and landed upstream (so that our patch will be obsolete when we next update ICU).

Local patches are stored in the ``intl/icu-patches`` directory. They’re applied when ICU is updated, so merely updating ICU files in place won’t persist changes across an ICU update.

Updating imported code
----------------------

The process of updating imported i18n-relevant code is *semi*-automated. We use a series of shell and Python scripts to do the job.

Updating ICU
~~~~~~~~~~~~

New ICU versions are announced on the `icu-announce <https://lists.sourceforge.net/lists/listinfo/icu-announce>`__ mailing list. Both release candidates and actual releases are announced here. It’s a good idea to attempt to update ICU when a release candidate is announced, just in case some serious problem is present (especially one that would be painful to fix through local patching).

``intl/update-icu.sh`` updates our ICU to a given ICU release: [#icu-git-argument]_

.. code:: bash

   $ cd "$topsrcdir/intl"
   $ # Ensure certain Python modules in the tree are accessible when updating.
   $ export PYTHONPATH="$topsrcdir/python/mozbuild/"
   $ #               <URL to ICU Git>                       <release tag name>
   $ ./update-icu.sh https://github.com/unicode-org/icu.git release-67-1

.. [#icu-git-argument]
   The ICU Git URL argument lets you update from a local ICU clone. This can speed up work when you’re updating to a new ICU release and need to adjust or add new local patches.

But usually you’ll want to update to the latest commit from the corresponding ICU maintenance branch so that you pick up fixes landed post-release:

.. code:: bash

   $ cd "$topsrcdir/intl"
   $ # Ensure certain Python modules in the tree are accessible when updating.
   $ export PYTHONPATH="$topsrcdir/python/mozbuild/"
   $ #               <URL to ICU Git>                       <maintenance name>
   $ ./update-icu.sh https://github.com/unicode-org/icu.git maint/maint-67

Updating ICU will also update the language tag registry (which records language tag semantics needed to correctly implement ``Intl`` functionality). Therefore it’s likely necessary to update SpiderMonkey’s language tag handling after running this [#update-icu-warning-langtags]_. See below where the ``langtags`` mode of ``make_intl_data.py`` is discussed.

.. [#update-icu-warning-langtags]
   ``update-icu.sh`` will print a notice as a reminder of this:

   .. code:: bash

      INFO: Please run 'js/src/builtin/intl/make_intl_data.py langtags' to update additional language tag files for SpiderMonkey.

``update-icu.sh`` is intended for *replayability*, not for hands-off runnability. It downloads ICU source, prunes various irrelevant files, replaces ``intl/icu/source`` with the new files – and then blindly applies local patches in fixed order.

Often a local patch won’t apply, or new patches must be applied to successfully build. In this case you’ll have to manually edit ``update-icu.sh`` to abort after only *some* patches have been applied, make whatever changes are necessary by hand, generate a new/updated patch file by hand, then carefully reattempt updating. (The people who have updated ICU in the past, usually jwalden and anba, follow this awkward process and don’t have good ideas on how to improve it.)

Any time ICU is updated, you’ll need to fully rebuild whichever of SpiderMonkey or Gecko you’re building. For SpiderMonkey, delete your object directory and reconfigure from scratch. For Gecko, change the message in the top-level `CLOBBER <https://searchfox.org/mozilla-central/source/CLOBBER>`__ file.

Updating tzdata
~~~~~~~~~~~~~~~

ICU contains a copy of ``tzdata``, but that copy is whatever ``tzdata`` release was current at the time the ICU release was finalized. Time zone data changes much more often than that: every time some national legislature or tinpot dictator decides to alter time zones. [#tzdata-release-frequency]_ The `tz-announce <https://mm.icann.org/pipermail/tz-announce/>`__ mailing list announces changes as they occur. (Note that we can’t *immediately* update when a release occurs: ICU’s `icu-data <https://github.com/unicode-org/icu-data>`__ repository must be updated before we can update our ``tzdata``.)

.. [#tzdata-release-frequency]
   To give a sense of how frequently ``tzdata`` is updated, and the irregularity of releases over time:

   -  2019 had three ``tzdata`` releases, 2019a through 2019c.
   -  2018 had nine ``tzdata`` releases, 2018a through 2018i.
   -  2017 had three ``tzdata`` releases, 2017a through 2017c.

Therefore, either (usually) after you update ICU *or* when a new ``tzdata`` release occurs, you’ll need to update our imported ``tzdata`` files. (If you do need to update time zone data, note that you’ll also need to additionally update SpiderMonkey’s time zone handling, described further below.) This also suitably updates ``config/external/icu/data/icudtNNE.dat``. (If you’ve just run ``update-icu.sh``, it will warn you that you need to do this. [#update-icu-warning-old-tzdata]_)

.. [#update-icu-warning-old-tzdata]
   For example:

   ::

      WARN: Local tzdata (2020a) is newer than ICU tzdata (2019c), please run './update-tzdata.sh 2020a'

First, make sure you have a usable ``icupkg`` on your system. [#icupkg-on-system]_ Then run the ``update-tzdata.sh`` script to update ``intl/tzdata`` and ``icudtNNE.dat``:

.. code:: bash

   $ cd "$topsrcdir/intl"
   $ ./update-tzdata.sh 2020a # or whatever the latest release is

.. [#icupkg-on-system]
   To install ``icupkg`` on your system:

   -  On Fedora, use ``sudo dnf install icu``.
   -  On Ubuntu, use ``sudo apt-get install icu-devtools``.
   -  On Mac OS X, use ``brew install icu4c``.
   -  On Windows, you’ll need to `download a binary build of ICU for Windows <https://github.com/unicode-org/icu/releases/tag/release-67-1>`__ and use the ``bin/icupkg.exe`` or ``bin64/icupkg.exe`` utility inside it.

   If you’re on Windows, or for some reason you don’t want to use the ``icupkg`` now in your ``$PATH``, you can manually specify it on the command line using the ``-e /path/to/icupkg`` flag:

   .. code:: bash

      $ cd "$topsrcdir/intl"
      $ ./update-tzdata.sh -e /path/to/icupkg 2020a # or whatever the latest release is

   *In principle*, the ``icupkg`` you use *should* be the one from the ICU release/maintenance branch being built: if there’s a mismatch, you might encounter an ICU “format version not supported” error. If you’re on Windows, make sure to download a binary build for that release/branch. On other platforms, you might have to build your own ICU from source. The steps required to do this are left as an exercise for the reader. (In the somewhat longer term, the update commands might be changed to do this themselves.)

If ``tzdata`` must be updated on trunk, you’ll almost certainly have to backport the update to Beta and ESR. Don’t attempt to backport the literal patch; just run the appropriate commands documented here to do so.

Updating SpiderMonkey ``Intl`` data
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

SpiderMonkey itself can’t blindly invoke ICU to perform every i18n operation, because sometimes ICU behavior deviates from what web specifications require. Therefore, when ICU is updated, we also must update SpiderMonkey itself as well (including various generated tests). Such updating is performed using the various modes of ``js/src/builtin/make_intl_data.py``.

Updating SpiderMonkey time zone handling
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The ECMAScript Internationalization API requires that time zone identifiers (``America/New_York``, ``Antarctica/McMurdo``, etc.) be interpreted according to `IANA <https://www.iana.org/time-zones>`__ semantics. Unfortunately, ICU doesn’t precisely implement those semantics. (See comments in ``js/src/builtin/intl/SharedIntlData.h`` for details.) Therefore SpiderMonkey has to do certain pre- and post-processing based on what’s in IANA but not in ICU, and what’s in ICU that isn’t in IANA.

Use ``make_intl_data.py``\ ’s ``tzdata`` mode to update time zone information:

.. code:: bash

   $ cd "$topsrcdir/js/src/builtin/intl"
   $ # make_intl_data.py requires yaml.
   $ export PYTHONPATH="$topsrcdir/third_party/python/PyYAML/lib3/"
   $ python3 ./make_intl_data.py tzdata

The ``tzdata`` mode accepts two optional arguments that generally will not be needed:

-  **``--tz``** will act using data from a local ``tzdata/`` directory containing raw ``tzdata`` source (note that this is *not* the same as what is in ``intl/tzdata/source``). It may be useful to help debug problems that arise during an update.
-  **``--ignore-backzone``** will omit time zone information before 1970. SpiderMonkey and Gecko include this information by default. However, because (by deliberate policy) ``tzdata`` information before 1970 is not reliable to the same degree as data since 1970, and backzone data has a size cost, a SpiderMonkey embedding or custom Gecko build might decide to omit it.

Updating SpiderMonkey language tag handling
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Language tags (``en``, ``de-CH``, ``ar-u-ca-islamicc``, and so on) are the primary means of specifying localization characteristics. The ECMAScript Internationalization API supports certain operations that depend upon the current state of the language tag registry (stored in the Unicode Common Locale Data Repository, CLDR, a repository of all locale-specific characteristics) that specifies subtag semantics:

-  ``Intl.getCanonicalLocales`` and ``Intl.Locale`` must replace alias subtags with their preferred forms. For example, ``ar-u-ca-islamic-civil`` uses the preferred Islamic calendar subtag, while ``ar-u-ca-islamicc`` uses an alias.
-  ``Intl.Locale.prototype.maximize`` and ``Intl.Locale.prototype.minimize`` accept a language tag and add or remove “likely” subtags from it. For example, ``de`` most likely refers to German using Latin script in Germany, so it maximizes to ``de-Latn-DE`` – and in reverse, ``de-Latn-DE`` minimizes to simply ``de``.

These decisions vary over time: as countries change [#soviet-union]_, as customs change, as language prevalence in regions varies, etc.

.. [#soviet-union]
   For just one relevant example, the breakup of the Soviet Union is the cause of numerous entries in the language tag registry. ``ru-SU``, Russian as used in the Soviet Union, is now expressed as ``ru-RU``, Russian as used in Russia; ``ab-SU``, Abkhazian as used in the Soviet Union, is now expressed as ``ab-GE``, Abkhazian as used in Georgia; and so on for all the other satellite states.

Use ``make_intl_data.py``\ ’s ``langtags`` mode to update language tag information to the same CLDR version used by ICU:

.. code:: bash

   $ cd "$topsrcdir/js/src/builtin/intl"
   $ # make_intl_data.py requires yaml.
   $ export PYTHONPATH="$topsrcdir/third_party/python/PyYAML/lib3/"
   $ python3 ./make_intl_data.py langtags

The CLDR version used will be printed in the header of CLDR-sensitive generated files. For example, ``js/src/builtin/intl/LanguageTagGenerated.cpp`` currently begins with:

.. code:: cpp

   // Generated by make_intl_data.py. DO NOT EDIT.
   // Version: CLDR-37
   // URL: https://unicode.org/Public/cldr/37/core.zip

Updating SpiderMonkey currency support
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Currencies use different numbers of fractional digits in their preferred formatting. Most currencies use two decimal digits; a handful use no fractional digits or some other number. Currency fractional digit is maintained by ISO and must be updated as currencies change their preferred fractional digits or new currencies arise that don’t use two decimal digits.

Currency updates are fairly uncommon, so it’ll be rare to need to update currency info. A `newsletter <https://www.currency-iso.org/en/home/amendments/newsletter.html>`__ periodically sends updates about changes.

Use ``make_intl_data.py``\ ’s ``currency`` mode to update currency fractional digit information:

.. code:: bash

   $ cd "$topsrcdir/js/src/builtin/intl"
   $ # make_intl_data.py requires yaml.
   $ export PYTHONPATH="$topsrcdir/third_party/python/PyYAML/lib3/"
   $ python3 ./make_intl_data.py currency

Updating SpiderMonkey measurement formatting support
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The ``Intl`` API supports formatting numbers as measurement units (for example, “17 meters” or “42 meters per second”). It specifies a list of units that must be supported, that we centrally record in ``js/src/builtin/intl/SanctionedSimpleUnitIdentifiers.yaml``, that we verify are supported by ICU and generate supporting files from.

If ``Intl``\ ’s list of supported units is ever updated, two separate changes will be required.

First, ``intl/icu/data_filter.json`` must be updated to incorporate localized strings for the new unit. These strings are stored in ``icudtNNE.dat``, so you’ll have to re-update ICU (and likely reimport ``tzdata`` as well, if it’s been updated since the last ICU update) to rewrite that file.

Second, use ``make_intl_data.py``\ ’s ``units`` mode to update unit handling and associated tests in SpiderMonkey:

.. code:: bash

   $ cd "$topsrcdir/js/src/builtin/intl"
   $ # make_intl_data.py requires yaml.
   $ export PYTHONPATH="$topsrcdir/third_party/python/PyYAML/lib3/"
   $ python3 ./make_intl_data.py units

Updating SpiderMonkey numbering systems support
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The ``Intl`` API also supports formatting numbers in various numbering systems (for example, “123“ using Latin numbers or “一二三“ using Han decimal numbers). The list of numbering systems that we must support is stored in ``js/src/builtin/intl/NumberingSystems.yaml``. We verify these numbering systems are supported by ICU and generate supporting files from it.

When the list of supported numbering systems needs to be updated, run ``make_intl_data.py`` with the ``numbering`` mode to update it and associated tests in SpiderMonkey:

.. code:: bash

   $ cd "$topsrcdir/js/src/builtin/intl"
   $ # make_intl_data.py requires yaml.
   $ export PYTHONPATH="$topsrcdir/third_party/python/PyYAML/lib3/"
   $ python3 ./make_intl_data.py numbering

