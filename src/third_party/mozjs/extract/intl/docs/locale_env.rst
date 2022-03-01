Environments
============

While all the concepts described above apply to all programming languages and frameworks
used by Mozilla, there are differences in completeness of the implementation.

Below is the current list of APIs supported in each environment and examples of how to
use them:

C++
---

In C++ the core API for Locale is :js:`mozilla::intl::Locale` and the service for locale
management is :js:`mozilla::intl::LocaleService`.

For any OSPreference operations there's :js:`mozilla::intl::OSPreferences`.


JavaScript
----------

In JavaScript users can use :js:`mozilla.org/intl/mozILocaleService` XPCOM API to access
the LocaleService and :js:`mozilla.org/intl/mozIOSPreferences` for OS preferences.

The LocaleService API is exposed as :js:`Services.locale` object.

There's currently no API available for operations on language tags and Locale objects,
but `Intl.Locale`_ API is in the works.

Rust
----

For Rust Mozilla provides a crate `fluent-locale`_ which implements the concepts described
above.

.. _Intl.Locale: https://bugzilla.mozilla.org/show_bug.cgi?id=1433303
.. _fluent-locale: https://docs.rs/fluent-locale/
