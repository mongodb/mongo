/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* ES6 20140718 draft 19.5.3.4. */
function ErrorToString()
{
  /* Steps 1-2. */
  var obj = this;
  if (!IsObject(obj))
    ThrowError(JSMSG_INCOMPATIBLE_PROTO, "Error", "toString", "value");

  /* Steps 3-5. */
  var name = obj.name;
  name = (name === undefined) ? "Error" : ToString(name);

  /* Steps 6-8. */
  var msg = obj.message;
  msg = (msg === undefined) ? "" : ToString(msg);

  /* Step 9. */
  if (name === "")
    return msg;

  /* Step 10. */
  if (msg === "")
    return name;

  /* Step 11. */
  return name + ": " + msg;
}
