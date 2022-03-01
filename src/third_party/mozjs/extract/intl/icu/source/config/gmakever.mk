## -*-makefile-*-
# Copyright (C) 2016 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html
#******************************************************************************
#   Copyright (C) 2008-2011, International Business Machines
#   Corporation and others.  All Rights Reserved.
#******************************************************************************
# Make sure we have the right version of Make.

at_least=3.80
ifeq ($(MACHTYPE),i370-ibm-mvs)
at_least=3.79.1
endif
ifeq ($(PLATFORM),OS390)
at_least=3.79.1
endif
ifeq ($(MACHTYPE),powerpc-ibm-os400)
at_least=3.77
endif

latest_a=$(firstword $(sort $(MAKE_VERSION) $(at_least)))

ifneq ($(at_least),$(latest_a))
err:
	@echo "ERROR: $(MAKE_VERSION) - too old, please upgrade to at least $(at_least)"
	@false
else
ok:
	@echo "$(MAKE_VERSION) (we wanted at least $(at_least))"
endif

