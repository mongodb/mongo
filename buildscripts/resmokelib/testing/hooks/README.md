# Hooks

All hooks inherit from the `buildscripts.resmokelib.testing.hooks.interface.Hook` parent class and
can override any subset of the following empty base methods: `before_suite`, `after_suite`,
`before_test`, `after_test`. At least 1 base method must be overridden, otherwise the hook will
not do anything at all. During test suite execution, each hook runs its custom logic in the
respective scenarios. Some customizable tasks that hooks can perform include: _validating data,
deleting data, performing cleanup_, etc.
