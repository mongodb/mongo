---------------------------------- MODULE MCTxnsCollectionIncarnation ------------------------------
\* This module defines TxnsCollectionIncarnation.tla constants/constraints for model-checking.

EXTENDS TxnsCollectionIncarnation
CONSTANTS DDLS
ASSUME DDLS \in 1..100

\* Allow one more DDL than specified in the constant to explore any pending transaction actions.
MAX_CLUSTER_TIME == INITIAL_CLUSTER_TIME + DDLS + 1

(**************************************************************************************************)
(* State Constraints.                                                                             *)
(**************************************************************************************************)

StateConstraint ==
    \* Stop state space exploration once all transactions are done, as our invariants are about 
    \* transaction result correctness, it is pointless to continue when all transactions are done.
    /\ \E t \in Txns : rCompletedStmt[t] < TXN_STMTS  
    \* Cap exploration to the MAX_CLUSTER_TIME, which is defined in terms of allowed DDLS.
    /\ clusterTime < MAX_CLUSTER_TIME

\* Defining symmetry sets for our model values allows TLC to avoid exploring equivalent states.                   
Symmetry == Permutations(Shards) \union Permutations(NameSpaces) \union Permutations(Keys) \union Permutations(Txns)

(**************************************************************************************************)
(* Counterexamples.                                                                               *)
(**************************************************************************************************)

\* Produces a counterexample trace where SIN happens
BaitSIN == 
    ~ \E t \in Txns, stm \in Stmts: 
        /\ HasResponse(response[t][stm])
        /\ \E rsp \in response[t][stm]: rsp.status = SNAPSHOT_INCOMPATIBLE

\* Produces a counterexample trace where everything goes perfect, all txn committed
BaitHappyPath ==
    <> \E t \in Txns, stm \in Stmts:
        /\ HasResponse(response[t][stm]) 
        /\ \A rsp \in response[t][stm]: rsp.status # OK  

\* Produces a counterexample trace of length >=N
BaitTrace == 
    TLCGet("level") < 155

====================================================================================================