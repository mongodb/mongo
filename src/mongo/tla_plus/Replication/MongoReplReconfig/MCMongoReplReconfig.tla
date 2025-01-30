---- MODULE MCMongoReplReconfig ----
\* This module defines MCMongoReplReconfig.tla constants/constraints for model-checking.

EXTENDS MongoReplReconfig

(**************************************************************************************************)
(* State Constraint. Used for model checking only.                                                *)
(**************************************************************************************************)

CONSTANTS MaxTerm, MaxLogLen, MaxConfigVersion, MaxCommittedEntries

StateConstraint == \A s \in Server :
                    /\ currentTerm[s] <= MaxTerm
                    /\ Len(log[s]) <= MaxLogLen
                    /\ configVersion[s] <= MaxConfigVersion
                    /\ Cardinality(immediatelyCommitted) <= MaxCommittedEntries

ServerSymmetry == Permutations(Server)
=============================================================================
