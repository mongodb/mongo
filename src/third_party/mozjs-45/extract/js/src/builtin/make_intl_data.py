#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

""" Usage: make_intl_data.py [language-subtag-registry.txt]

    This script extracts information about mappings between deprecated and
    current BCP 47 language tags from the IANA Language Subtag Registry and
    converts it to JavaScript object definitions in IntlData.js. The definitions
    are used in Intl.js.

    The IANA Language Subtag Registry is imported from
    http://www.iana.org/assignments/language-subtag-registry
    and uses the syntax specified in
    http://tools.ietf.org/html/rfc5646#section-3
"""

def readRegistryRecord(registry):
    """ Yields the records of the IANA Language Subtag Registry as dictionaries. """
    record = {}
    for line in registry:
        line = line.strip()
        if line == "":
            continue
        if line == "%%":
            yield record
            record = {}
        else:
            if ":" in line:
                key, value = line.split(":", 1)
                key, value = key.strip(), value.strip()
                record[key] = value
            else:
                # continuation line
                record[key] += " " + line
    if record:
        yield record
    return


def readRegistry(registry):
    """ Reads IANA Language Subtag Registry and extracts information for Intl.js.

        Information extracted:
        - langTagMappings: mappings from complete language tags to preferred
          complete language tags
        - langSubtagMappings: mappings from subtags to preferred subtags
        - extlangMappings: mappings from extlang subtags to preferred subtags,
          with prefix to be removed
        Returns these three mappings as dictionaries, along with the registry's
        file date.

        We also check that mappings for language subtags don't affect extlang
        subtags and vice versa, so that CanonicalizeLanguageTag doesn't have
        to separate them for processing. Region codes are separated by case,
        and script codes by length, so they're unproblematic.
    """
    langTagMappings = {}
    langSubtagMappings = {}
    extlangMappings = {}
    languageSubtags = set()
    extlangSubtags = set()

    for record in readRegistryRecord(registry):
        if "File-Date" in record:
            fileDate = record["File-Date"]
            continue

        if record["Type"] == "grandfathered":
            # Grandfathered tags don't use standard syntax, so
            # CanonicalizeLanguageTag expects the mapping table to provide
            # the final form for all.
            # For langTagMappings, keys must be in lower case; values in
            # the case used in the registry.
            tag = record["Tag"]
            if "Preferred-Value" in record:
                langTagMappings[tag.lower()] = record["Preferred-Value"]
            else:
                langTagMappings[tag.lower()] = tag
        elif record["Type"] == "redundant":
            # For langTagMappings, keys must be in lower case; values in
            # the case used in the registry.
            if "Preferred-Value" in record:
                langTagMappings[record["Tag"].lower()] = record["Preferred-Value"]
        elif record["Type"] in ("language", "script", "region", "variant"):
            # For langSubtagMappings, keys and values must be in the case used
            # in the registry.
            subtag = record["Subtag"]
            if record["Type"] == "language":
                languageSubtags.add(subtag)
            if "Preferred-Value" in record:
                if subtag == "heploc":
                    # The entry for heploc is unique in its complexity; handle
                    # it as special case below.
                    continue
                if "Prefix" in record:
                    # This might indicate another heploc-like complex case.
                    raise Exception("Please evaluate: subtag mapping with prefix value.")
                langSubtagMappings[subtag] = record["Preferred-Value"]
        elif record["Type"] == "extlang":
            # For extlangMappings, keys must be in the case used in the
            # registry; values are records with the preferred value and the
            # prefix to be removed.
            subtag = record["Subtag"]
            extlangSubtags.add(subtag)
            if "Preferred-Value" in record:
                preferred = record["Preferred-Value"]
                prefix = record["Prefix"]
                extlangMappings[subtag] = {"preferred": preferred, "prefix": prefix}
        else:
            # No other types are allowed by
            # http://tools.ietf.org/html/rfc5646#section-3.1.3
            assert False, "Unrecognized Type: {0}".format(record["Type"])

    # Check that mappings for language subtags and extlang subtags don't affect
    # each other.
    for lang in languageSubtags:
        if lang in extlangMappings and extlangMappings[lang]["preferred"] != lang:
            raise Exception("Conflict: lang with extlang mapping: " + lang)
    for extlang in extlangSubtags:
        if extlang in langSubtagMappings:
            raise Exception("Conflict: extlang with lang mapping: " + extlang)

    # Special case for heploc.
    langTagMappings["ja-latn-hepburn-heploc"] = "ja-Latn-alalc97"

    return {"fileDate": fileDate,
            "langTagMappings": langTagMappings,
            "langSubtagMappings": langSubtagMappings,
            "extlangMappings": extlangMappings}


def writeMappingsVar(intlData, dict, name, description, fileDate, url):
    """ Writes a variable definition with a mapping table to file intlData.

        Writes the contents of dictionary dict to file intlData with the given
        variable name and a comment with description, fileDate, and URL.
    """
    intlData.write("\n")
    intlData.write("// {0}.\n".format(description))
    intlData.write("// Derived from IANA Language Subtag Registry, file date {0}.\n".format(fileDate))
    intlData.write("// {0}\n".format(url))
    intlData.write("var {0} = {{\n".format(name))
    keys = sorted(dict)
    for key in keys:
        if isinstance(dict[key], basestring):
            value = '"{0}"'.format(dict[key])
        else:
            preferred = dict[key]["preferred"]
            prefix = dict[key]["prefix"]
            value = '{{preferred: "{0}", prefix: "{1}"}}'.format(preferred, prefix)
        intlData.write('    "{0}": {1},\n'.format(key, value))
    intlData.write("};\n")


def writeLanguageTagData(intlData, fileDate, url, langTagMappings, langSubtagMappings, extlangMappings):
    """ Writes the language tag data to the Intl data file. """
    writeMappingsVar(intlData, langTagMappings, "langTagMappings",
                     "Mappings from complete tags to preferred values", fileDate, url)
    writeMappingsVar(intlData, langSubtagMappings, "langSubtagMappings",
                     "Mappings from non-extlang subtags to preferred values", fileDate, url)
    writeMappingsVar(intlData, extlangMappings, "extlangMappings",
                     "Mappings from extlang subtags to preferred values", fileDate, url)


if __name__ == '__main__':
    import codecs
    import sys
    import urllib2

    url = "http://www.iana.org/assignments/language-subtag-registry"
    if len(sys.argv) > 1:
        print("Always make sure you have the newest language-subtag-registry.txt!")
        registry = codecs.open(sys.argv[1], "r", encoding="utf-8")
    else:
        print("Downloading IANA Language Subtag Registry...")
        reader = urllib2.urlopen(url)
        text = reader.read().decode("utf-8")
        reader.close()
        registry = codecs.open("language-subtag-registry.txt", "w+", encoding="utf-8")
        registry.write(text)
        registry.seek(0)

    print("Processing IANA Language Subtag Registry...")
    data = readRegistry(registry)
    fileDate = data["fileDate"]
    langTagMappings = data["langTagMappings"]
    langSubtagMappings = data["langSubtagMappings"]
    extlangMappings = data["extlangMappings"]
    registry.close()

    print("Writing Intl data...")
    intlData = codecs.open("IntlData.js", "w", encoding="utf-8")
    intlData.write("// Generated by make_intl_data.py. DO NOT EDIT.\n")
    writeLanguageTagData(intlData, fileDate, url, langTagMappings, langSubtagMappings, extlangMappings)
    intlData.close()
