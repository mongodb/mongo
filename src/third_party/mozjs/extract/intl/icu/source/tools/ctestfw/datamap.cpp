// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/********************************************************************
 * COPYRIGHT: 
 * Copyright (c) 2002-2006, International Business Machines Corporation and
 * others. All Rights Reserved.
 ********************************************************************/

/* Created by weiv 05/09/2002 */

#include "unicode/datamap.h"
#include "unicode/resbund.h"
#include "unicode/unistr.h"
#include "hash.h"
#include <stdlib.h>

DataMap::~DataMap() {}
DataMap::DataMap() {}

int32_t 
DataMap::utoi(const UnicodeString &s) const
{
  char ch[256];
  const UChar *u = toUCharPtr(s.getBuffer());
  int32_t len = s.length();
  u_UCharsToChars(u, ch, len);
  ch[len] = 0; /* include terminating \0 */
  return atoi(ch);
}

U_CDECL_BEGIN
void U_CALLCONV
deleteResBund(void *obj) {
  delete (ResourceBundle *)obj;
}
U_CDECL_END


RBDataMap::~RBDataMap()
{
  delete fData;
}

RBDataMap::RBDataMap()
{
  UErrorCode status = U_ZERO_ERROR;
  fData = new Hashtable(TRUE, status);
  fData->setValueDeleter(deleteResBund);
}

// init from table resource
// will put stuff in hashtable according to 
// keys.
RBDataMap::RBDataMap(UResourceBundle *data, UErrorCode &status)
{
  fData = new Hashtable(TRUE, status);
  fData->setValueDeleter(deleteResBund);
  init(data, status);
}

// init from headers and resource
// with checking the whether the size of resource matches 
// header size
RBDataMap::RBDataMap(UResourceBundle *headers, UResourceBundle *data, UErrorCode &status) 
{
  fData = new Hashtable(TRUE, status);
  fData->setValueDeleter(deleteResBund);
  init(headers, data, status);
}


void RBDataMap::init(UResourceBundle *data, UErrorCode &status) {
  int32_t i = 0;
  fData->removeAll();
  UResourceBundle *t = NULL;
  for(i = 0; i < ures_getSize(data); i++) {
    t = ures_getByIndex(data, i, t, &status);
    fData->put(UnicodeString(ures_getKey(t), -1, US_INV), new ResourceBundle(t, status), status);
  }
  ures_close(t);
}

void RBDataMap::init(UResourceBundle *headers, UResourceBundle *data, UErrorCode &status)
{
  int32_t i = 0;
  fData->removeAll();
  UResourceBundle *t = NULL;
  const UChar *key = NULL;
  int32_t keyLen = 0;
  if(ures_getSize(headers) == ures_getSize(data)) {
    for(i = 0; i < ures_getSize(data); i++) {
      t = ures_getByIndex(data, i, t, &status);
      key = ures_getStringByIndex(headers, i, &keyLen, &status);
      fData->put(UnicodeString(key, keyLen), new ResourceBundle(t, status), status);
    }
  } else {
    // error
    status = U_INVALID_FORMAT_ERROR;
  }
  ures_close(t);
}

const ResourceBundle *RBDataMap::getItem(const char* key, UErrorCode &status) const
{
  if(U_FAILURE(status)) {
    return NULL;
  }

  UnicodeString hashKey(key, -1, US_INV);
  const ResourceBundle *r = (ResourceBundle *)fData->get(hashKey);
  if(r != NULL) {
    return r;
  } else {
    status = U_MISSING_RESOURCE_ERROR;
    return NULL;
  }
}

const UnicodeString RBDataMap::getString(const char* key, UErrorCode &status) const
{
  const ResourceBundle *r = getItem(key, status);
  if(U_SUCCESS(status)) {
    return r->getString(status);
  } else {
    return UnicodeString();
  }
}

int32_t
RBDataMap::getInt28(const char* key, UErrorCode &status) const
{
  const ResourceBundle *r = getItem(key, status);
  if(U_SUCCESS(status)) {
    return r->getInt(status);
  } else {
    return 0;
  }
}

uint32_t
RBDataMap::getUInt28(const char* key, UErrorCode &status) const
{
  const ResourceBundle *r = getItem(key, status);
  if(U_SUCCESS(status)) {
    return r->getUInt(status);
  } else {
    return 0;
  }
}

const int32_t *
RBDataMap::getIntVector(int32_t &length, const char *key, UErrorCode &status) const {
  const ResourceBundle *r = getItem(key, status);
  if(U_SUCCESS(status)) {
    return r->getIntVector(length, status);
  } else {
    return NULL;
  }
}

const uint8_t *
RBDataMap::getBinary(int32_t &length, const char *key, UErrorCode &status) const {
  const ResourceBundle *r = getItem(key, status);
  if(U_SUCCESS(status)) {
    return r->getBinary(length, status);
  } else {
    return NULL;
  }
}

int32_t RBDataMap::getInt(const char* key, UErrorCode &status) const
{
  UnicodeString r = this->getString(key, status);
  if(U_SUCCESS(status)) {
    return utoi(r);
  } else {
    return 0;
  }
}

const UnicodeString* RBDataMap::getStringArray(int32_t& count, const char* key, UErrorCode &status) const 
{
  const ResourceBundle *r = getItem(key, status);
  if(U_SUCCESS(status)) {
    int32_t i = 0;

    count = r->getSize();
    if(count <= 0) {
      return NULL;
    }

    UnicodeString *result = new UnicodeString[count];
    for(i = 0; i<count; i++) {
      result[i] = r->getStringEx(i, status);
    }
    return result;
  } else {
    return NULL;
  }
}

const int32_t* RBDataMap::getIntArray(int32_t& count, const char* key, UErrorCode &status) const
{
  const ResourceBundle *r = getItem(key, status);
  if(U_SUCCESS(status)) {
    int32_t i = 0;

    count = r->getSize();
    if(count <= 0) {
      return NULL;
    }

    int32_t *result = new int32_t[count];
    UnicodeString stringRes;
    for(i = 0; i<count; i++) {
      stringRes = r->getStringEx(i, status);
      result[i] = utoi(stringRes);
    }
    return result;
  } else {
    return NULL;
  }
}

