---- MODULE MCMultiTenantMigrations ----
\* This module defines MCMultiTenantMigrations.tla constants/constraints for model-checking.

EXTENDS MultiTenantMigrations

CONSTANT MaxRequests

(**************************************************************************************************)
(* State Constraint. Used for model checking only.                                                *)
(**************************************************************************************************)

StateConstraint ==
    MaxRequests > totalRequests

=============================================================================
