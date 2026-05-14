------------------------------ MODULE MCStandbyStepUpRace --------------------------------
\* Model-checking module for StandbyStepUpRace. See StandbyStepUpRace.tla.

EXTENDS StandbyStepUpRace

(**************************************************************************************************)
(* Initial-state overrides.                                                                       *)
(*                                                                                                *)
(* The base Init lets fixEnabled range over BOOLEAN so a single spec admits     *)
(* both worlds. The two MC cfgs use these initial predicates to pin the world    *)
(* under test:                                                                  *)
(*   - InitFix    : fixEnabled = TRUE  (the fix is in; invariants must hold)    *)
(*   - InitBug    : fixEnabled = FALSE (the buggy world; counter-examples shown)*)
(**************************************************************************************************)

InitFix ==
    /\ role = Secondary
    /\ standby = StandbyRunning
    /\ stepUpHolder = NONE
    /\ stepUpPhase = "idle"
    /\ callbackQueue = <<>>
    /\ grpcFatal = FALSE
    /\ fixEnabled = TRUE

InitBug ==
    /\ role = Secondary
    /\ standby = StandbyRunning
    /\ stepUpHolder = NONE
    /\ stepUpPhase = "idle"
    /\ callbackQueue = <<>>
    /\ grpcFatal = FALSE
    /\ fixEnabled = FALSE

SpecFix == InitFix /\ [][Next]_vars
    /\ \A w \in Workers : WF_vars(StepUpJoin(w))
    /\ \A w \in Workers : WF_vars(StepUpAcquireRSTL(w))
    /\ \A w \in Workers : WF_vars(StepUpBecomePrimary(w))
    /\ \A w \in Workers : WF_vars(StepUpRelease(w))

SpecBug == InitBug /\ [][Next]_vars
    /\ \A w \in Workers : WF_vars(StepUpJoin(w))
    /\ \A w \in Workers : WF_vars(StepUpAcquireRSTL(w))
    /\ \A w \in Workers : WF_vars(StepUpBecomePrimary(w))
    /\ \A w \in Workers : WF_vars(StepUpRelease(w))

(**************************************************************************************************)
(* State constraints to keep the model finite.                                                   *)
(**************************************************************************************************)

\* Hard bound on the callback queue (also enforced inline in the actions; this
\* is the global state-space cap for the model checker).
QueueLimit == Len(callbackQueue) <= 3

\* Once a fatal has been reached we stop generating new events; the fatal
\* state is absorbing in the bug cfg and we don't need TLC to explore beyond.
NoMoreEventsAfterFatal == grpcFatal => callbackQueue = <<>>

StateConstraint == QueueLimit /\ NoMoreEventsAfterFatal

(**************************************************************************************************)
(* Counterexamples / bait invariants.                                                            *)
(*                                                                                                *)
(* In the BUG cfg, BaitNoFatal is supplied as an invariant; TLC reports a       *)
(* counter-example trace ending in grpcFatal = TRUE -- the standby/becomePrimary*)
(* race materialised. Under the FIX cfg, BaitNoFatal is redundant with          *)
(* NoGrpcFatal but kept here so the two cfgs differ only in which Init / Spec   *)
(* they bind.                                                                   *)
(**************************************************************************************************)

BaitNoFatal == ~grpcFatal

=================================================================================================
