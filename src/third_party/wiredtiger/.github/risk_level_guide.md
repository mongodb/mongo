## Risk level guide

**Low risk**: with high confidence the change won’t unexpectedly change the behavior, or the change touches a non-critical area.

**Medium risk**: with medium confidence the change won’t unexpectedly change the behavior, or the change touches an area you are not familiar with or easy to break.

**High risk**: can't tell a clear story about what tests give us confidence that the change won’t unexpectedly change the behavior.

Try asking yourself a few questions to figure out (or elaborate on) the risk level:

- Does the change touch the core logic?
  - If not (e.g. test-only/tooling/compile changes), it’s likely low-risk.
- Does the change touch a WT API used by server?
  - If yes, it’s medium+ risk.
- Does the change fix a concurrency issue?
  - If yes, it’s medium+ risk.
- Does the change touch an area you are not familiar with?
  - If yes, it’s medium+ risk.
- Does the change touch a module of low code coverage?
  - If yes, it’s medium+ risk.
- Could the change lead to any performance implications?
  - If yes, it’s medium+ risk.
- If more than 1 answer to the above questions are "yes" (i.e. medium+ risk), it’s likely high risk.
