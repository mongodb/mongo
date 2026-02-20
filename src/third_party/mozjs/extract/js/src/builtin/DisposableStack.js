/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Explicit Resource Management Proposal
// 27.3.3.3 DisposableStack.prototype.dispose ( )
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-disposablestack.prototype.dispose
function $DisposableStackDispose() {
  // Step 1. Let disposableStack be the this value.
  var disposableStack = this;

  if (!IsObject(disposableStack) || (disposableStack = GuardToDisposableStackHelper(disposableStack)) === null) {
    return callFunction(
      CallDisposableStackMethodIfWrapped,
      this,
      "$DisposableStackDispose"
    );
  }

  // Step 2. Perform ? RequireInternalSlot(disposableStack, [[DisposableState]]).
  var state = UnsafeGetReservedSlot(disposableStack, DISPOSABLE_STACK_STATE_SLOT);

  // Step 3. If disposableStack.[[DisposableState]] is disposed, return undefined.
  if (state === DISPOSABLE_STACK_STATE_DISPOSED) {
    return undefined;
  }

  // Step 4. Set disposableStack.[[DisposableState]] to disposed.
  UnsafeSetReservedSlot(disposableStack, DISPOSABLE_STACK_STATE_SLOT, DISPOSABLE_STACK_STATE_DISPOSED);

  // Step 5. Return ? DisposeResources(disposableStack.[[DisposeCapability]], NormalCompletion(undefined)).
  var disposeCapability = UnsafeGetReservedSlot(disposableStack, DISPOSABLE_STACK_DISPOSABLE_RESOURCE_STACK_SLOT);
  UnsafeSetReservedSlot(disposableStack, DISPOSABLE_STACK_DISPOSABLE_RESOURCE_STACK_SLOT, undefined);
  // disposeCapability being undefined indicates that its an empty stack, thus
  // all the following operations are no-ops, hence we can return early.
  if (disposeCapability === undefined) {
    return undefined;
  }
  DisposeResourcesSync(disposeCapability, disposeCapability.length);

  return undefined;
}
SetCanonicalName($DisposableStackDispose, "dispose");
