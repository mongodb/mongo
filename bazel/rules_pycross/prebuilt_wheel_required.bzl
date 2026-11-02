"""Reject source fallback for packages supplied as prebuilt wheels."""

def _prebuilt_wheel_required_impl(ctx):
    fail("{} requires a compatible prebuilt wheel in uv.lock; source builds are disabled. Publish the missing wheel and update the root lock.".format(ctx.attr.package))

prebuilt_wheel_required = rule(
    implementation = _prebuilt_wheel_required_impl,
    attrs = {"package": attr.string(mandatory = True)},
)
