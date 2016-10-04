/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef devtools_Instruments_h
#define devtools_Instruments_h

#ifdef __APPLE__

#include <unistd.h>

namespace Instruments {

bool Start(pid_t pid);
void Pause();
bool Resume();
void Stop(const char* profileName);

}

#endif /* __APPLE__ */

#endif /* devtools_Instruments_h */
