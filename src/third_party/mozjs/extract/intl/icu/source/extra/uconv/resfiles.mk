# Copyright (C) 2016 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html
# Copyright (c) 2000-2002 IBM, Inc. and Others.
# A small makefile containing the list of resource bundles 
# to include in uconv.

# The variable FILESEPCHAR is defined by the caller to be
# the character separating components of a filename.

RESOURCESDIR = resources
RESSRC = $(RESOURCESDIR)$(FILESEPCHAR)root.txt $(RESOURCESDIR)$(FILESEPCHAR)fr.txt
