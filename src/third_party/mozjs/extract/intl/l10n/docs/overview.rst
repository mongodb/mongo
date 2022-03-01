.. role:: js(code)
   :language: javascript

============
Localization
============

Localization at Mozilla
=======================

At Mozilla localizations are managed by locale communities around the world, who
are responsible for maintaining high quality linguistic and cultural adaptation
of Mozilla software into over 100 locales.

The exact process of localization management differs from project to project, but
in the case of Gecko applications, the localization is primarily done via a web localization
system called `Pontoon`_ and stored in HG repositories under
`hg.mozilla.org/l10n-central`_.

Developers are expected to keep their code localizable using localization
and internationalization systems, and also serve as localizers into the `en-US` locale
which is used as the `source` locale.

In between the developers and localizers, there's a sophisticated ecosystem of tools,
tests, automation, validators and other checks on one hand, and management, release,
community and quality processes facilitated by the `L10n Drivers Team`_, on the other.

Content vs. UI
==============

The two main categories in localization are content localization vs UI localization.

The former is usually involved when dealing with large blocks of text such as
documentation, help articles, marketing material and legal documents.

The latter is the primary type when handling user interfaces for applications such
as Firefox.

This article will focus on UI localization.

Lifecycle & Workflow
====================

1) New feature
--------------

The typical life cycle of a localizable UI starts with a UX/UI or new feature need which
should be accompanied by the UX mockups involving so called `copy` - the original
content to be used in the new piece of UI.

2) UX mockup + copy review
--------------------------

The UX mockup with copy is the first step that should be reviewed by the L10n Drivers Team.
Their aim is to identify potential cultural and localization challenges that may arise
later and ensure that the UI is ready for localization on a linguistic, cultural,
and technical level.

3) Patch l10n review
--------------------

Once that is completed, the next stage is for front-end engineers to create patches
which implement the new UI. Those patches should already contain the `copy` and
place the strings in the localization resources for the source locale (`en-US`).

The developer uses the localization API by selecting a special identifier we call
`L10n ID` and optionally a list of variables that will be passed to the translation.

We call this "a social contract" which binds the l10n-id/args combination to a particular
source translation to use in the UI.

The localizer expects the developer to maintain the contract by ensuring that the
translation will be used in the given location, and will correspond to the
source translation. If that contract is to be changed, the developer will be expected
to update it. More on that in part `6) String Updates`.

The next review comes from either L10n Drivers, or experienced front end engineers
familiar with the internationalization and localization systems, making sure that
the patches properly use the right APIs and the code is ready to be landed
into `mozilla-central`.

.. _exposure-in-gecko-strings:

4) Exposure in `gecko-strings`
------------------------------

Once the patch lands in `mozilla-central`, L10n Drivers will take a final look at
the localizability of the introduced strings. In case of issues, developers might
be asked to land a follow up, or the patch could be backed out with the help of sheriffs.

Every few days, strings are exported into a repository called `gecko-strings-quarantine`,
a unified repository that includes strings for all shipping versions of Firefox
(nightly, beta, release). This repository is used as a buffer to avoid exposing potential
issues to over 100 locales.

As a last step, strings are pushed into `gecko-strings`, another unified repository that
is exposed to localization tools, like Pontoon, and build automation.

5) Localization
---------------

From that moment localizers will work on providing translations for the new feature
either while the new strings are only in Nightly or after they are merged to Beta.
The goal is to have as much of the UI ready in as many locales as early as possible,
but the process is continuous and we're capable of releasing Firefox with incomplete
translations falling back on a backup locale in case of a missing string.

While Nightly products use the latest version of localization available in repositories,
the L10n Drivers team is responsible for reviewing and signing off versions of each
localization shipping in Beta and Release versions of Gecko products.

6) String updates
-----------------

Later in the software life cycle some strings might need to be changed or removed.
As a general rule, once the strings lands in `mozilla-central`, any further update
to existing strings will need to follow these guidelines, independently from how much
time has passed since previous changes.

If it's just a string removal, all the engineer has to do is to remove it from the UI
and from the localization resource file in `mozilla-central`.

If it's an update, we currently have two "levels" of change severity:

1) If the change is minor, like fixing a spelling error or case, the developer should update
the `en-US` translation without changing the l10n-id.

2) If the change is anyhow affecting the meaning or tone of the message, the developer
is requested to update the l10n string ID.

The latter is considered a change in the social contract between the developer and
the localizer and an update to the ID is expected.

In case of `Fluent`_, any changes to the structure of the message such as adding/removing
attributes also requires an update of the ID.

The new ID will be recognized by the l10n tooling as untranslated, and the old one
as obsolete. This will give the localizers an opportunity to find and update the
translation, while the old string will be removed from the build process.

There is a gray area between the two severity levels. In case of doubt, don’t hesitate
to request feedback of review from L10n Drivers to avoid issues once the strings land.

Selecting L10n Identifier
=========================

Choosing an identifier for a localization message is tricky. It may seem similar
to picking a variable name, but in reality, it's much closer to designing a public
API.

An l10n identifier, once defined, is then getting associated to a translated
message in every one of 100+ locales and it becomes very costly to attempt to
migrate that string in all locales to a different identifier.

Additionally, in Fluent an identifier is used as a last resort string to be displayed in
an error scenario when formatting the message fails, which makes selecting
**meaningful** identifiers particularly valuable.

Lastly, l10n resources get mixed and matched into localization contexts where
it becomes important to avoid identifier collision from two strings coming
from two different files.

For all those reasons, a longer identifier such as :js:`privacy-exceptions-button-ok` is
preferred over short identifiers like :js:`ok` or :js:`ok-button`.

Localization Systems
====================

Gecko has three main localization systems: Fluent and two legacy systems,
DTD and StringBundle.

Fluent
------

Fluent is a modern localization system designed by Mozilla to address the challenges
and limitations of older systems.

It's well suited for modern web development cycle, provides a number of localization
features including good internationalization model and strong bidirectionality support.


To learn more about Fluent, follow the `Fluent for Firefox Developers`_ guide.

DTD & StringBundle
------------------

DTD are deprecated, but still used for XUL and XHTML file localization. It uses `.dtd` files
and the only localization feature it provides is the ability to reference one
string from another via entity reference.

StringBundle is a runtime API used primarily for localization of C++ code.
The messages are stored in `.properties` files and loaded using the StringBundle API
and then retrieved from there via imperative calls.

The system provides external arguments which can be placed into the string, and
support basic plural categories via a proprietary API `PluralForms.jsm`.

.. _Pontoon: https://pontoon.mozilla.org/
.. _hg.mozilla.org/l10n-central: https://hg.mozilla.org/l10n-central/
.. _L10n Drivers Team: https://wiki.mozilla.org/L10n:Mozilla_Team
.. _Fluent For Firefox Developers: ./l10n/l10n/fluent_tutorial.html
