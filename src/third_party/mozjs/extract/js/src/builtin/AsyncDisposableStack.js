/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Explicit Resource Management
// 27.4.3.3 AsyncDisposableStack.prototype.disposeAsync ( )
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-asyncdisposablestack.prototype.disposeAsync
async function $AsyncDisposableStackDisposeAsync() {
  // Step 1. Let asyncDisposableStack be the this value.
  var asyncDisposableStack = this;

  if (!IsObject(asyncDisposableStack) || (asyncDisposableStack = GuardToAsyncDisposableStackHelper(asyncDisposableStack)) === null) {
    return callFunction(
      CallAsyncDisposableStackMethodIfWrapped,
      this,
      "$AsyncDisposableStackDisposeAsync"
    );
  }

  // Step 2. Let promiseCapability be ! NewPromiseCapability(%Promise%).
  // (implicit)
  // Step 3. If asyncDisposableStack does not have an [[AsyncDisposableState]] internal slot, then
  var state = UnsafeGetReservedSlot(asyncDisposableStack, DISPOSABLE_STACK_STATE_SLOT);
  if (state === undefined) {
    // Step 3.a. Perform ! Call(promiseCapability.[[Reject]], undefined, « a newly created TypeError object »).
    // Step 3.b. Return promiseCapability.[[Promise]].
    // (implicit)
    ThrowTypeError(JSMSG_INCOMPATIBLE_METHOD, 'disposeAsync', 'method', 'AsyncDisposableStack');
  }

  // Step 4. If asyncDisposableStack.[[AsyncDisposableState]] is disposed, then
  if (state === DISPOSABLE_STACK_STATE_DISPOSED) {
    // Step 4.a. Perform ! Call(promiseCapability.[[Resolve]], undefined, « undefined »).
    // Step 4.b. Return promiseCapability.[[Promise]].
    return undefined;
  }

  // Step 5. Set asyncDisposableStack.[[AsyncDisposableState]] to disposed.
  UnsafeSetReservedSlot(asyncDisposableStack, DISPOSABLE_STACK_STATE_SLOT, DISPOSABLE_STACK_STATE_DISPOSED);

  // Step 6. Let result be Completion(DisposeResources(asyncDisposableStack.[[DisposeCapability]], NormalCompletion(undefined))).
  // Step 7. IfAbruptRejectPromise(result, promiseCapability).
  var disposeCapability = UnsafeGetReservedSlot(asyncDisposableStack, DISPOSABLE_STACK_DISPOSABLE_RESOURCE_STACK_SLOT);
  UnsafeSetReservedSlot(asyncDisposableStack, DISPOSABLE_STACK_DISPOSABLE_RESOURCE_STACK_SLOT, undefined);
  if (disposeCapability === undefined) {
    return undefined;
  }
  DisposeResourcesAsync(disposeCapability, disposeCapability.length);

  // Step 8. Perform ! Call(promiseCapability.[[Resolve]], undefined, « result »).
  // Step 9. Return promiseCapability.[[Promise]].
  return undefined;
}
SetCanonicalName($AsyncDisposableStackDisposeAsync, "disposeAsync");
