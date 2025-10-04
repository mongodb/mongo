/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_StableCellHasher_h
#define gc_StableCellHasher_h

#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

// StableCellHasher itself is defined in js/public/RootingAPI.h.

namespace js::gc {

struct Cell;

// Gets an existing UID in |uidp| if one exists.
[[nodiscard]] bool MaybeGetUniqueId(Cell* cell, uint64_t* uidp);

// Puts an existing UID in |uidp|, or creates a new UID for this Cell and
// puts that into |uidp|. Returns false on OOM.
[[nodiscard]] bool GetOrCreateUniqueId(Cell* cell, uint64_t* uidp);

uint64_t GetUniqueIdInfallible(Cell* cell);

// Return true if this cell has a UID associated with it.
[[nodiscard]] bool HasUniqueId(Cell* cell);

// Transfer an id from another cell. This must only be called on behalf of a
// moving GC. This method is infallible.
void TransferUniqueId(Cell* tgt, Cell* src);

// Remove any unique id associated with this Cell.
void RemoveUniqueId(Cell* cell);

// Used to restore unique ID after JSObject::swap.
bool SetOrUpdateUniqueId(JSContext* cx, Cell* cell, uint64_t uid);

}  // namespace js::gc

#endif  // gc_StableCellHasher_h
