// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
 ******************************************************************************
 *
 *   Copyright (C) 1998-2006, International Business Machines
 *   Corporation and others.  All Rights Reserved.
 *
 ******************************************************************************
 *
 *
 *  ucnvstat.c:
 *  UConverterStaticData prototypes for data based converters
 */

#include "unicode/utypes.h"
#include "unicode/ucnv.h"
#include "toolutil.h"
#include "ucnv_bld.h"


static const UConverterStaticData _SBCSStaticData={
    sizeof(UConverterStaticData),
    "SBCS",
    0, UCNV_IBM, UCNV_SBCS, 1, 1,
    { 0x1a, 0, 0, 0 }, 1, FALSE, FALSE,
    0,
    0,
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } /* reserved */
};


static const UConverterStaticData _DBCSStaticData={
    sizeof(UConverterStaticData),
    "DBCS",
    0, UCNV_IBM, UCNV_DBCS, 2, 2,
    { 0, 0, 0, 0 },0, FALSE, FALSE, /* subchar */
    0,
    0,
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } /* reserved */
};

static const UConverterStaticData _MBCSStaticData={
    sizeof(UConverterStaticData),
    "MBCS",
    0, UCNV_IBM, UCNV_MBCS, 1, 1,
    { 0x1a, 0, 0, 0 }, 1, FALSE, FALSE,
    0,
    0,
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } /* reserved */
};

static const UConverterStaticData _EBCDICStatefulStaticData={
    sizeof(UConverterStaticData),
    "EBCDICStateful",
    0, UCNV_IBM, UCNV_EBCDIC_STATEFUL, 1, 1,
    { 0, 0, 0, 0 },0, FALSE, FALSE,
    0,
    0,
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } /* reserved */
};

/* NULLs for algorithmic types, their tables live in ucnv_bld.c */
const UConverterStaticData *ucnv_converterStaticData[UCNV_NUMBER_OF_SUPPORTED_CONVERTER_TYPES]={
    &_SBCSStaticData, &_DBCSStaticData, &_MBCSStaticData, NULL/*Lat1*/,
    NULL/*UTF8*/, NULL/*UTF16be*/, NULL/*UTF16LE*/, NULL/*UTF32be*/, NULL/*UTF32LE*/, &_EBCDICStatefulStaticData,
    NULL/*ISO2022*/,
    /* LMBCS */ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

