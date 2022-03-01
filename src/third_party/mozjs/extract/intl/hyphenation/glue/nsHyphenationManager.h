/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsHyphenationManager_h__
#define nsHyphenationManager_h__

#include "base/shared_memory.h"
#include "mozilla/Omnijar.h"
#include "nsHashKeys.h"
#include "nsInterfaceHashtable.h"
#include "nsIObserver.h"
#include "nsRefPtrHashtable.h"

class nsHyphenator;
class nsAtom;
class nsIURI;

class nsHyphenationManager : public nsIObserver {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER

  nsHyphenationManager();

  already_AddRefed<nsHyphenator> GetHyphenator(nsAtom* aLocale);

  void ShareHyphDictToProcess(nsIURI* aURI, base::ProcessId aPid,
                              base::SharedMemoryHandle* aOutHandle,
                              uint32_t* aOutSize);

  static nsHyphenationManager* Instance();

  static void Shutdown();

  size_t SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf);

 private:
  virtual ~nsHyphenationManager();

 protected:
  void LoadPatternList();
  void LoadPatternListFromOmnijar(mozilla::Omnijar::Type aType);
  void LoadPatternListFromDir(nsIFile* aDir);
  void LoadAliases();

  nsRefPtrHashtable<nsRefPtrHashKey<nsAtom>, nsAtom> mHyphAliases;
  nsInterfaceHashtable<nsRefPtrHashKey<nsAtom>, nsIURI> mPatternFiles;
  nsRefPtrHashtable<nsRefPtrHashKey<nsAtom>, nsHyphenator> mHyphenators;

  static nsHyphenationManager* sInstance;
};

#endif  // nsHyphenationManager_h__
