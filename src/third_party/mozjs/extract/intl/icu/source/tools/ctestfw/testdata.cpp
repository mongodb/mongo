// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/********************************************************************
 * COPYRIGHT: 
 * Copyright (c) 2002-2005, International Business Machines Corporation and
 * others. All Rights Reserved.
 ********************************************************************/

/* Created by weiv 05/09/2002 */

#include "unicode/testdata.h"


TestData::TestData(const char* testName)
: name(testName),
fInfo(NULL),
fCurrSettings(NULL),
fCurrCase(NULL),
fSettingsSize(0),
fCasesSize(0),
fCurrentSettings(0),
fCurrentCase(0)

{
}

TestData::~TestData() {
  if(fInfo != NULL) {
    delete fInfo;
  }
  if(fCurrSettings != NULL) {
    delete fCurrSettings;
  }
  if(fCurrCase != NULL) {
    delete fCurrCase;
  }
}

const char * TestData::getName() const
{
  return name;
}



RBTestData::RBTestData(const char* testName)
: TestData(testName),
fData(NULL),
fHeaders(NULL),
fSettings(NULL),
fCases(NULL)
{
}

RBTestData::RBTestData(UResourceBundle *data, UResourceBundle *headers, UErrorCode& status)
: TestData(ures_getKey(data)),
fData(data),
fHeaders(headers),
fSettings(NULL),
fCases(NULL)
{
  UErrorCode intStatus = U_ZERO_ERROR;
  UResourceBundle *currHeaders = ures_getByKey(data, "Headers", NULL, &intStatus);
  if(intStatus == U_ZERO_ERROR) {
    ures_close(fHeaders);
    fHeaders = currHeaders;
  } else {
    intStatus = U_ZERO_ERROR;
  }
  fSettings = ures_getByKey(data, "Settings", NULL, &intStatus);
  fSettingsSize = ures_getSize(fSettings);
  UResourceBundle *info = ures_getByKey(data, "Info", NULL, &intStatus);
  if(U_SUCCESS(intStatus)) {
    fInfo = new RBDataMap(info, status);
  } else {
    intStatus = U_ZERO_ERROR;
  }
  fCases = ures_getByKey(data, "Cases", NULL, &status);
  fCasesSize = ures_getSize(fCases);

  ures_close(info);
}


RBTestData::~RBTestData()
{
  ures_close(fData);
  ures_close(fHeaders);
  ures_close(fSettings);
  ures_close(fCases);
}

UBool RBTestData::getInfo(const DataMap *& info, UErrorCode &/*status*/) const
{
  if(fInfo) {
    info = fInfo;
    return TRUE;
  } else {
    info = NULL;
    return FALSE;
  }
}

UBool RBTestData::nextSettings(const DataMap *& settings, UErrorCode &status)
{
  UErrorCode intStatus = U_ZERO_ERROR;
  UResourceBundle *data = ures_getByIndex(fSettings, fCurrentSettings++, NULL, &intStatus);
  if(U_SUCCESS(intStatus)) {
    // reset the cases iterator
    fCurrentCase = 0;
    if(fCurrSettings == NULL) {
      fCurrSettings = new RBDataMap(data, status);
    } else {
      ((RBDataMap *)fCurrSettings)->init(data, status);
    }
    ures_close(data);
    settings = fCurrSettings;
    return TRUE;
  } else {
    settings = NULL;
    return FALSE;
  }
}

UBool RBTestData::nextCase(const DataMap *& nextCase, UErrorCode &status)
{
  UErrorCode intStatus = U_ZERO_ERROR;
  UResourceBundle *currCase = ures_getByIndex(fCases, fCurrentCase++, NULL, &intStatus);
  if(U_SUCCESS(intStatus)) {
    if(fCurrCase == NULL) {
      fCurrCase = new RBDataMap(fHeaders, currCase, status);
    } else {
      ((RBDataMap *)fCurrCase)->init(fHeaders, currCase, status);
    }
    ures_close(currCase);
    nextCase = fCurrCase;
    return TRUE;
  } else {
    nextCase = NULL;
    return FALSE;
  }
}


