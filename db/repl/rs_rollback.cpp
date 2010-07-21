/* @file rs_rollback.cpp
* 
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "../client.h"
#include "../../client/dbclient.h"
#include "rs.h"
#include "../repl.h"

/* Scenarios

   We went offline with ops not replicated out.
 
       F = node that failed and coming back.
       P = node that took over, new primary

   #1:
       F : a b c d e f g
       P : a b c d q

   The design is "keep P".  One could argue here that "keep F" has some merits, however, in most cases P 
   will have significantly more data.  Also note that P may have a proper subset of F's stream if there were 
   no subsequent writes!

   For now the model is simply : get F back in sync with P.  If P was really behind or something, we should have 
   just chosen not to fail over anyway.

   #2:
       F : a b c d e f g                -> a b c d
       P : a b c d

   #3:
       F : a b c d e f g                -> a b c d q r s t u v w x z
       P : a b c d.q r s t u v w x z

   Steps
    find an event in common. 'd'.
    undo our events beyond that by: 
      (1) taking copy from other server of those objects
      (2) do not consider copy valid until we pass the original end point (e.g. g) in time
          -- i.e., reset minvalid.
      (3) skip operations on objects that are previous in time to our capture of the object.

a b c d e f
a b c
a b c d e.g 

*/

namespace mongo {

  void ReplSetImpl::syncRollback(OplogReader&r, ...) { 
  }

}
