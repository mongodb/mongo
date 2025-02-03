# Introduction

Content to be added by [SERVER-98887](https://jira.mongodb.org/browse/SERVER-98887)

# SBE Plan Representation

Content to be added by [SERVER-98888](https://jira.mongodb.org/browse/SERVER-98888), and should
include the following topics (for example):

- PlanStage
- SlotId
- EExpression

# SBE Plan Preparation

Content to be added by [SERVER-98889](https://jira.mongodb.org/browse/SERVER-98889), and should
include the following topics (for example):

- Bytecode compilation
- getAccessor()

# SBE Runtime

Content to be added by [SERVER-98890](https://jira.mongodb.org/browse/SERVER-98890), and should
include the following topics (for example):

- SBE values and ownership
- Kinds of accessors
- Iterator model: open(), getNext(), close()
- VM
  - Description of stack-based model
  - “Instruction functions” versus builtin functions
  - Lambdas
- RuntimeEnvironment

# SBE Stage Builders

Content to be added by [SERVER-98891](https://jira.mongodb.org/browse/SERVER-98891), and should
include the following topics (for example):

- Bottom-up QuerySolution -> SBE compilation process
- PlanStageSlots and PlanStageReqs.
- Use of ABT/IR.
