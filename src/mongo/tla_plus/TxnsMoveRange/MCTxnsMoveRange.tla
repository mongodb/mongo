---------------------------------- MODULE MCTxnsMoveRange ------------------------------------------
\* This module defines TxnsMoveRange.tla constants/constraints for model-checking.

EXTENDS TxnsMoveRange

(**************************************************************************************************)
(* State Constraints.                                                                             *)
(**************************************************************************************************)

StateConstraint == /\ \E t \in Txns : rCompletedStmt[t] < TXN_STMTS

(**************************************************************************************************)
(* Counterexamples.                                                                               *)
(**************************************************************************************************)

\* Produces a snapshotIncompatible counterexample.
BaitSnapshotIncompatible ==
    ~(\E t \in Txns, stm \in Stmts:
        /\ HasResponse(response[t][stm])
        /\ response[t][stm]["rsp"] = snapshotIncompatible)

\* Produces a counterexample trace where all transactions commit.
BaitHappyPath ==
    <>(\E t \in Txns, stm \in Stmts:
        /\ HasResponse(response[t][stm])
        /\ response[t][stm]["rsp"] \in {snapshotIncompatible, staleRouter})

\* Produces a counterexample trace of length >=N.
BaitTrace == TLCGet("level") < 155

====================================================================================================
