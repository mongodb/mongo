====================
Internationalization
====================

Internationalization (`"i18n"`) is a domain of computer science focused on making
software accessible across languages, regions and cultures.
A combination of those is called a `locale`.

On the most abstract level, Gecko internationalization is a set of algorithms,
data structures and APIs that aim to enable Gecko to work with all human scripts and
languages, both as a UI toolkit and as a web engine.

In order to achieve that, i18n has to hook into many components such as layout, gfx, dom,
widget, build, front-end, JS engine and accessibility.
It also has to be available across programming languages and frameworks used in the
platform and front-end.

Below is a list of articles that introduce the concepts necessary to understand and
use Mozilla's I18n APIs.

.. toctree::
   :maxdepth: 1

   locale
   dataintl
   icu
