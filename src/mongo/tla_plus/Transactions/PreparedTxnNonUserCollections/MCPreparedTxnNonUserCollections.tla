---------------------------- MODULE MCPreparedTxnNonUserCollections ------------------------------
\* Model-check overlay for PreparedTxnNonUserCollections. Caps the state space
\* and declares the invariant set wired into the .cfg file.

EXTENDS PreparedTxnNonUserCollections

\* Symmetry over namespaces and transactions: lock identifiers and txn
\* identifiers are interchangeable from the spec's perspective.
Symmetry ==
    Permutations(UserNamespaces)
        \union Permutations(NonUserNamespaces)
        \union Permutations(Txns)

\* Cap exploration once every transaction has reached a terminal state.
StateConstraint ==
    \E t \in Txns : txnState[t] \in {NotStarted, Prepared, Reclaimed}

====================================================================================================
