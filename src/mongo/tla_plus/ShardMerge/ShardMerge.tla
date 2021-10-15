\* Copyright 2021-present MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

----------------------------- MODULE ShardMerge -----------------------------
\*
\* A specification of serverless MongoDB's shard merge protocol.
\*
\* To run the model-checker, first edit the constants in MCShardMerge.cfg if desired,
\* then:
\*     cd src/mongo/db/repl/tla_plus
\*     ./model-check.sh ShardMerge
\*

EXTENDS Integers, FiniteSets, Sequences, TLC

\* Donor command requests and responses
CONSTANTS DonorStartMigrationRequest, DonorStartMigrationResponse
CONSTANTS DonorForgetMigrationRequest, DonorForgetMigrationResponse

\* recipientSyncData command with returnAfterPinningOldestTimestamp.
CONSTANTS RecipientSyncDataReturnAfterPinningRequest, RecipientSyncDataReturnAfterPinningResponse
\* recipientSyncData command with no special params.
CONSTANTS RecipientSyncDataRequest, RecipientSyncDataResponse
\* recipientSyncData command with returnAfterReachingDonorTimestamp.
CONSTANTS RecipientSyncDataReturnAfterReachingDonorTimestampRequest, RecipientSyncDataReturnAfterReachingDonorTimestampResponse
CONSTANTS RecipientForgetMigrationRequest, RecipientForgetMigrationResponse

\* Recipient states. The happy path is:
\* Uninitialized->Pinned->Started->Consistent->Lagged->Ready->Done.
CONSTANTS RecUninitialized, RecPinned, RecStarted, RecConsistent, RecLagged, RecReady, RecAborted, RecDone

\* Donor states. The happy path is:
\* Uninit->AbortingIndexBuilds->Pinning->DataSync->Blocking->Committed->Done.
CONSTANTS DonUninitialized, DonAbortingIndexBuilds, DonPinning, DonDataSync, DonBlocking, DonCommitted, DonAborted, DonDone

\* cloud state
CONSTANTS CloudUnknown, CloudCommitted, CloudAborted, CloudDone

\* Responses to DonorStartMigration request
CONSTANTS MigrationNone, MigrationCommitted, MigrationAborted

\* Responses to RecipientSyncData* requests
CONSTANTS SyncOK, SyncAborted

(**************************************************************************************************)
(* Global variables                                                                               *)
(**************************************************************************************************)

VARIABLE messages
VARIABLE recipientState
VARIABLE donorState
VARIABLE cloudState
VARIABLE totalRequests
VARIABLE totalResponses
VARIABLE recipientAborted

donorVars == <<donorState>>
recipientVars == <<recipientState, recipientAborted>>
cloudVars == <<cloudState>>
messageVars == <<messages, totalRequests, totalResponses>>
vars == <<donorVars, recipientVars, cloudVars, messageVars>>

-------------------------------------------------------------------------------------------

(**************************************************************************************************)
(* Network Helpers, adapted from https://github.com/ongardie/raft.tla/blob/master/raft.tla        *)
(**************************************************************************************************)

\* Helper for Send. Given a message m and bag of messages, return a new bag of messages with one
\* more m in it.
WithMessage(m, msgs) ==
    IF m \in DOMAIN msgs THEN
        [msgs EXCEPT ![m] = msgs[m] + 1]
    ELSE
        msgs @@ (m :> 1)

\* Helper for Discard and Reply. Given a message m and bag of messages, return a new bag of
\* messages with one less m in it.
WithoutMessage(m, msgs) ==
    IF m \in DOMAIN msgs THEN
        IF msgs[m] = 1 THEN
            \* Remove message m from the bag.
            [n \in DOMAIN msgs \ {m} |-> msgs[n]]
        ELSE
            [msgs EXCEPT ![m] = msgs[m] - 1]
    ELSE
        msgs

IsRequest(m) ==
   m.mType \in {DonorStartMigrationRequest, RecipientSyncDataReturnAfterPinningRequest,
            RecipientSyncDataRequest, RecipientSyncDataReturnAfterReachingDonorTimestampRequest,
            DonorForgetMigrationRequest, RecipientForgetMigrationRequest}

IncTotalMessages(m) ==
    IF IsRequest(m) THEN
        /\ totalRequests' = totalRequests + 1
        /\ UNCHANGED <<totalResponses>>
    ELSE
        /\ totalResponses' = totalResponses + 1
        /\ UNCHANGED <<totalRequests>>

\* Add a message to the bag of messages.
Send(m) ==
    /\ messages' = WithMessage(m, messages)
    /\ IncTotalMessages(m)

\* Remove a message from the bag of messages. Used when a server is done processing a message.
Discard(m) ==
    /\ messages' = WithoutMessage(m, messages)
    /\ UNCHANGED <<totalRequests, totalResponses>>

\* Helper that both sends a message and discards a message.
SendAndDiscard(sendMessage, discardMessage) ==
    /\ messages' = WithoutMessage(discardMessage, WithMessage(sendMessage, messages))
    /\ IncTotalMessages(sendMessage)

(**************************************************************************************************)
(* Request and response handlers                                                                  *)
(**************************************************************************************************)

\* Helper to create the donorStartMigration response based on the donor state.
DonorStartMigrationResponseGen ==
    CASE donorState = DonAborted ->
         [mType    |-> DonorStartMigrationResponse,
          mOutcome |-> MigrationAborted]
      [] donorState = DonCommitted ->
         [mType    |-> DonorStartMigrationResponse,
          mOutcome |-> MigrationCommitted]
      [] donorState \in {DonUninitialized, DonAbortingIndexBuilds, DonPinning, DonDataSync,
                         DonBlocking, DonDone} ->
         [mType    |-> DonorStartMigrationResponse,
          mOutcome |-> MigrationNone]

\* Donor
HandleDonorStartMigrationRequest(m) ==
    /\ m.mType = DonorStartMigrationRequest
    \* If the donor is unstarted, it starts, otherwise nothing happens. Either way sends a response
    \* to cloud.
    /\ CASE donorState = DonUninitialized ->
            /\ donorState' = DonAbortingIndexBuilds
            \* Send an immediate response to cloud.
            /\ SendAndDiscard(DonorStartMigrationResponseGen, m)
         [] donorState \in {DonAbortingIndexBuilds, DonPinning, DonDataSync, DonBlocking,
                            DonCommitted, DonAborted, DonDone} ->
            /\ SendAndDiscard(DonorStartMigrationResponseGen, m)
            /\ UNCHANGED <<donorVars>>
    /\ UNCHANGED <<recipientVars, cloudVars, totalRequests>>

\* Cloud
HandleDonorStartMigrationResponse(m) ==
    /\ m.mType = DonorStartMigrationResponse
    \* Updates the cloud state to whatever the donor specifies, if specified.
    /\ CASE m.mOutcome = MigrationNone ->
            UNCHANGED <<cloudState>>
         [] m.mOutcome = MigrationCommitted ->
            cloudState' = CloudCommitted
         [] m.mOutcome = MigrationAborted ->
            cloudState' = CloudAborted
    /\ Discard(m)
    /\ UNCHANGED <<donorVars, recipientVars>>

\* Helper to generate the mSyncStatus field of a recipient response
RecipientSyncStatusGen == IF recipientAborted THEN SyncAborted ELSE SyncOK

\* Recipient
HandleRecipientSyncDataReturnAfterPinningRequest(m) ==
    /\ m.mType = RecipientSyncDataReturnAfterPinningRequest
    /\ CASE recipientState = RecUninitialized ->
            recipientState' = RecPinned
         [] recipientState \in {RecPinned, RecStarted, RecConsistent,
                                RecLagged, RecReady, RecAborted, RecDone} ->
            UNCHANGED recipientState
    /\ SendAndDiscard([mType |-> RecipientSyncDataReturnAfterPinningResponse,
                       mSyncStatus |-> RecipientSyncStatusGen], m)
    /\ UNCHANGED <<recipientAborted, donorVars, cloudVars>>

\* Factored out of below to make nested Case statements clearer.
HandleRecipientSyncDataReturnAfterPinningResponse_SyncOK(m) ==
    CASE donorState = DonPinning ->
         \* Move the state machine to "data sync" and send RecipientSyncData
         /\ donorState' = DonDataSync
         /\ SendAndDiscard([mType |-> RecipientSyncDataRequest], m)
      [] donorState \in {DonDataSync, DonBlocking, DonCommitted, DonAborted, DonDone} ->
         \* Just ignore this message, since we're past this step in the protocol
         \* and this is a delayed message.
         /\ Discard(m)
         /\ UNCHANGED <<donorState>>

\* Factored out of below to make nested Case statements clearer.
HandleRecipientSyncDataReturnAfterPinningResponse_SyncAborted(m) ==
    /\ CASE donorState = DonPinning ->
            \* The recipient failed the migration, so abort.
            donorState' = DonAborted
         [] donorState \in {DonDataSync, DonBlocking, DonAborted, DonDone} ->
            \* Delayed response to an earlier message, ignore it.
            UNCHANGED <<donorState>>
    /\ Discard(m)

\* Donor
HandleRecipientSyncDataReturnAfterPinningResponse(m) ==
    /\ m.mType = RecipientSyncDataReturnAfterPinningResponse
    /\ CASE m.mSyncStatus = SyncOK ->
            HandleRecipientSyncDataReturnAfterPinningResponse_SyncOK(m)
         [] m.mSyncStatus = SyncAborted ->
            HandleRecipientSyncDataReturnAfterPinningResponse_SyncAborted(m)
    /\ UNCHANGED <<recipientVars, cloudVars>>

\* Recipient
HandleRecipientSyncDataRequest(m) ==
    /\ m.mType = RecipientSyncDataRequest
    \* Don't handle messages until we transition to consistent, or abort.
    /\ recipientState # RecStarted
    /\ Assert(recipientState # RecUninitialized,
              "Received RecipientSyncData in state "
              \o ToString(recipientState))
    /\ CASE recipientState = RecPinned ->
            \* Starts the migration. The recipient does not respond to the donor until it is
            \* consistent.
            /\ recipientState' = RecStarted
            /\ Discard(m)
            /\ UNCHANGED <<recipientAborted>>
         [] recipientState # RecPinned ->
            /\ SendAndDiscard([mType |-> RecipientSyncDataResponse,
                               mSyncStatus |-> RecipientSyncStatusGen], m)
            /\ UNCHANGED <<recipientVars>>
    /\ UNCHANGED <<donorVars, cloudVars>>

\* Factored out of below to make nested Case statements clearer.
HandleRecipientSyncDataResponse_SyncOK(m) ==
    /\ CASE donorState = DonDataSync ->
         \* Move the state machine to "blocking" and send RecipientSyncDataReturnAfterReachingDonorTimestamp.
         /\ donorState' = DonBlocking
         /\ SendAndDiscard([mType |-> RecipientSyncDataReturnAfterReachingDonorTimestampRequest], m)
      [] donorState \in {DonBlocking, DonCommitted, DonAborted, DonDone} ->
         \* Just ignore this message, since we're past this step in the protocol
         \* and this is a delayed message.
         /\ Discard(m)
         /\ UNCHANGED <<donorState>>

\* Factored out of below to make nested Case statements clearer.
HandleRecipientSyncDataResponse_SyncAborted(m) ==
    /\ CASE donorState \in {DonDataSync, DonBlocking} ->
            \* The recipient failed the migration, so abort.
            \* We can get this response in Blocking when there are two
            \* RecipientSyncData responses and the "OK" one is processed first.
            donorState' = DonAborted
         [] donorState \in {DonCommitted, DonAborted, DonDone} ->
            \* The migration is already finished, do nothing.
            UNCHANGED <<donorState>>
    /\ Discard(m)

\* Donor
HandleRecipientSyncDataResponse(m) ==
    /\ m.mType = RecipientSyncDataResponse
    /\ Assert(donorState \notin {DonUninitialized, DonPinning},
              "Received RecipientSyncDataResponse in state "
              \o ToString(donorState))
    /\ CASE m.mSyncStatus = SyncOK ->
            HandleRecipientSyncDataResponse_SyncOK(m)
         [] m.mSyncStatus = SyncAborted ->
            HandleRecipientSyncDataResponse_SyncAborted(m)
    /\ UNCHANGED <<recipientVars, cloudVars>>

\* Recipient
HandleRecipientSyncDataReturnAfterReachingDonorTimestampRequest(m) ==
    /\ m.mType = RecipientSyncDataReturnAfterReachingDonorTimestampRequest
    \* We don't want to handle this request being processed while lagged, since that would
    \* require modeling request joining behavior, which is unnecessary complexity for the
    \* purposes of this model. A RecipientSyncDataReturnAfterReachingDonorTimestamp request being
    \* processed while in RecLagged must be a duplicate message.
    /\ recipientState \notin {RecLagged}
    /\ CASE recipientState = RecConsistent ->
            \* Move the state machine to "lagged", since the recipient now knows the ending
            \* timestamp. The recipient does not respond to the donor until it has caught up.
            /\ recipientState' = RecLagged
            /\ Discard(m)
            /\ UNCHANGED <<recipientAborted>>
         [] recipientState # RecConsistent ->
            /\ SendAndDiscard([mType |-> RecipientSyncDataReturnAfterReachingDonorTimestampResponse,
                               mSyncStatus |-> RecipientSyncStatusGen], m)
            /\ UNCHANGED <<recipientVars>>
    /\ UNCHANGED <<donorVars, cloudVars>>

\* Factored out of below to make nested Case statements clearer.
HandleRecipientSyncDataReturnAfterReachingDonorTimestampResponse_SyncOK ==
    CASE donorState = DonBlocking ->
         \* The recipient is done!
         donorState' = DonCommitted
      [] donorState \in {DonCommitted, DonAborted, DonDone} ->
         \* Just ignore this message, since we're past this step in the protocol
         \* and this is a delayed message.
         UNCHANGED <<donorState>>

\* Factored out of below to make nested Case statements clearer.
HandleRecipientSyncDataReturnAfterReachingDonorTimestampResponse_SyncAborted ==
    CASE donorState = DonBlocking ->
         \*  The recipient failed the migration, so abort.
         donorState' = DonAborted
      [] donorState \in {DonAborted, DonDone} ->
         \* If the migration is already aborted or finished, do nothing.
         UNCHANGED <<donorState>>
\* Donor
HandleRecipientSyncDataReturnAfterReachingDonorTimestampResponse(m) ==
    /\ m.mType = RecipientSyncDataReturnAfterReachingDonorTimestampResponse
    /\ CASE m.mSyncStatus = SyncOK ->
            HandleRecipientSyncDataReturnAfterReachingDonorTimestampResponse_SyncOK
         [] m.mSyncStatus = SyncAborted ->
            HandleRecipientSyncDataReturnAfterReachingDonorTimestampResponse_SyncAborted
    /\ Discard(m)
    /\ UNCHANGED <<recipientVars, cloudVars>>

\* Donor
HandleDonorForgetMigrationRequest(m) ==
    /\ m.mType = DonorForgetMigrationRequest
    \* Don't mark donor finished until recipient is.
    /\ SendAndDiscard([mType |-> RecipientForgetMigrationRequest], m)
    /\ UNCHANGED <<donorVars, recipientVars, cloudVars>>

\* Cloud
HandleDonorForgetMigrationResponse(m) ==
    /\ m.mType = DonorForgetMigrationResponse
    \* The donor and recipient unconditionally finish the migration, so cloud can too.
    /\ cloudState' = CloudDone
    /\ Discard(m)
    /\ UNCHANGED <<donorVars, recipientVars>>

\* Recipient
HandleRecipientForgetMigrationRequest(m) ==
    /\ m.mType = RecipientForgetMigrationRequest
    \* Finish the migration no matter what, and tell the donor.
    /\ recipientState' = RecDone
    /\ SendAndDiscard([mType |-> RecipientForgetMigrationResponse], m)
    /\ UNCHANGED <<donorVars, cloudVars, recipientAborted>>

\* Donor
HandleRecipientForgetMigrationResponse(m) ==
    /\ m.mType = RecipientForgetMigrationResponse
    \* The recipient has finished the migration, so now the donor can finish the migration and
    \* respond to cloud that it has finished the migration.
    /\ donorState' = DonDone
    /\ SendAndDiscard([mType |-> DonorForgetMigrationResponse], m)
    /\ UNCHANGED <<recipientVars, cloudVars>>


(******************************************************************************)
(* [ACTION]                                                                   *)
(******************************************************************************)

DonorAbortsIndexBuilds ==
    /\ donorState = DonAbortingIndexBuilds
    /\ donorState' = DonPinning
    \* Call recipientSyncData with returnAfterPinningOldestTimestamp.
    /\ Send([mType |-> RecipientSyncDataReturnAfterPinningRequest])
    /\ UNCHANGED <<totalResponses, recipientVars, cloudVars>>

\* Models a retry of recipientSyncData with returnAfterPinningOldestTimestamp.
DonorSendsRecipientSyncDataReturnAfterPinningRequest ==
    /\ donorState = DonPinning
    /\ Send([mType |-> RecipientSyncDataReturnAfterPinningRequest])
    /\ UNCHANGED <<donorVars, recipientVars, cloudVars>>

\* Models the first try or a retry of recipientSyncData.
DonorSendsRecipientSyncDataRequest ==
    /\ donorState = DonDataSync
    /\ Send([mType |-> RecipientSyncDataRequest])
    /\ UNCHANGED <<donorVars, recipientVars, cloudVars>>

\* Models a retry of RecipientSyncDataReturnAfterReachingDonorTimestamp.
DonorSendsRecipientSyncDataReturnAfterReachingDonorTimestampRequest ==
    /\ donorState = DonBlocking
    /\ Send([mType |-> RecipientSyncDataReturnAfterReachingDonorTimestampRequest])
    /\ UNCHANGED <<donorVars, recipientVars, cloudVars>>

CloudSendsDonorStartMigrationRequest ==
    /\ cloudState = CloudUnknown
    /\ Send([mType |-> DonorStartMigrationRequest])
    /\ UNCHANGED <<donorVars, recipientVars, cloudVars>>

CloudSendsDonorForgetMigrationRequest ==
    /\ cloudState \in {CloudAborted, CloudCommitted}
    /\ Send([mType |-> DonorForgetMigrationRequest])
    /\ UNCHANGED <<donorVars, recipientVars, cloudVars>>

RecipientBecomesConsistent ==
    /\ recipientState = RecStarted
    /\ recipientState' = RecConsistent
    /\ Send([mType |-> RecipientSyncDataResponse,
             mSyncStatus |-> RecipientSyncStatusGen])
    /\ UNCHANGED <<donorVars, cloudVars, recipientAborted>>

RecipientCatchesUp ==
    /\ recipientState = RecLagged
    /\ recipientState' = RecReady
    /\ Send([mType |-> RecipientSyncDataReturnAfterReachingDonorTimestampResponse,
             mSyncStatus |-> RecipientSyncStatusGen])
    /\ UNCHANGED <<donorVars, cloudVars, recipientAborted>>

RecipientFailsMigration ==
    \* Recipient can't fail after it's ready, finished, or already aborted.
    /\ recipientState \notin {RecUninitialized, RecReady, RecAborted, RecDone}
    /\ recipientState' = RecAborted
    /\ recipientAborted' = TRUE
    /\ CASE recipientState = RecStarted ->
            \* The recipient has an active RecipientSyncData request.
            Send([mType |-> RecipientSyncDataResponse,
                    mSyncStatus |-> SyncAborted])
         [] recipientState = RecLagged ->
            \* When "lagged" the recipient has an active RecipientSyncDataReturnAfterReachingDonorTimestamp request.
            Send([mType |-> RecipientSyncDataReturnAfterReachingDonorTimestampResponse,
                  mSyncStatus |-> SyncAborted])
         [] recipientState \in {RecUninitialized, RecPinned, RecConsistent} ->
            \* No active donor request.
            UNCHANGED <<messageVars>>
    /\ UNCHANGED <<cloudVars, donorVars>>

(**************************************************************************************************)
(* Correctness Properties                                                                         *)
(**************************************************************************************************)

StateMachinesInconsistent ==
    \/ /\ cloudState = CloudCommitted
       /\ \/ recipientState \notin {RecReady, RecDone}
          \/ recipientAborted = TRUE
          \/ donorState \notin {DonCommitted, DonDone}
    \/ /\ donorState = DonCommitted
       /\ \/ recipientState \notin {RecReady, RecDone}
          \/ recipientAborted = TRUE

StateMachinesConsistent == ~StateMachinesInconsistent

(**************************************************************************************************)
(* Liveness properties                                                                            *)
(**************************************************************************************************)

\* Checks that the state machines eventually converge on terminating states.
MigrationEventuallyCompletes ==
    <> /\ recipientState = RecDone
       /\ donorState = DonDone
       /\ cloudState = CloudDone

\* Checks that if the bag fills up, it eventually empties.
MessageBagEventuallyEmpties ==
    Cardinality(DOMAIN messages) > 0 ~> Cardinality(DOMAIN messages) = 0

\* Checks that the number of totalRequests eventually equals the number of totalResponses,
\* and stays that way. This will always be right before termination.
EachRequestHasAResponse ==
    <>[] (totalRequests = totalResponses)

(**************************************************************************************************)
(* Spec definition                                                                                *)
(**************************************************************************************************)
Init ==
    /\ messages = [m \in {} |-> 0]
    /\ donorState = DonUninitialized
    /\ recipientState = RecUninitialized
    /\ cloudState = CloudUnknown
    /\ totalRequests = 0
    /\ totalResponses = 0
    /\ recipientAborted = FALSE

RecipientBecomesConsistentAction == RecipientBecomesConsistent
RecipientCatchesUpAction == RecipientCatchesUp
RecipientFailsMigrationAction == RecipientFailsMigration
CloudSendsDonorStartMigrationRequestAction == CloudSendsDonorStartMigrationRequest
CloudSendsDonorForgetMigrationRequestAction == CloudSendsDonorForgetMigrationRequest
DonorAbortsIndexBuildsAction == DonorAbortsIndexBuilds
DonorSendsRecipientSyncDataReturnAfterPinningRequestAction == DonorSendsRecipientSyncDataReturnAfterPinningRequest
DonorSendsRecipientSyncDataRequestAction == DonorSendsRecipientSyncDataRequest
DonorSendsRecipientSyncDataReturnAfterReachingDonorTimestampRequestAction == DonorSendsRecipientSyncDataReturnAfterReachingDonorTimestampRequest

ReceiveDonorStartMigrationRequestAction == \E m \in DOMAIN messages :
    HandleDonorStartMigrationRequest(m)
ReceiveDonorStartMigrationResponseAction == \E m \in DOMAIN messages :
    HandleDonorStartMigrationResponse(m)
ReceiveRecipientSyncDataReturnAfterPinningRequestAction == \E m \in DOMAIN messages :
    HandleRecipientSyncDataReturnAfterPinningRequest(m)
ReceiveRecipientSyncDataReturnAfterPinningResponseAction == \E m \in DOMAIN messages :
    HandleRecipientSyncDataReturnAfterPinningResponse(m)
ReceiveRecipientSyncDataRequestAction == \E m \in DOMAIN messages :
    HandleRecipientSyncDataRequest(m)
ReceiveRecipientSyncDataResponseAction == \E m \in DOMAIN messages :
    HandleRecipientSyncDataResponse(m)
ReceiveRecipientSyncDataReturnAfterReachingDonorTimestampRequestAction == \E m \in DOMAIN messages :
    HandleRecipientSyncDataReturnAfterReachingDonorTimestampRequest(m)
ReceiveRecipientSyncDataReturnAfterReachingDonorTimestampResponseAction == \E m \in DOMAIN messages :
    HandleRecipientSyncDataReturnAfterReachingDonorTimestampResponse(m)
ReceiveDonorForgetMigrationRequestAction == \E m \in DOMAIN messages :
    HandleDonorForgetMigrationRequest(m)
ReceiveDonorForgetMigrationResponseAction == \E m \in DOMAIN messages :
    HandleDonorForgetMigrationResponse(m)
ReceiveRecipientForgetMigrationRequestAction == \E m \in DOMAIN messages :
    HandleRecipientForgetMigrationRequest(m)
ReceiveRecipientForgetMigrationResponseAction == \E m \in DOMAIN messages :
    HandleRecipientForgetMigrationResponse(m)

Next ==
    \/ RecipientBecomesConsistentAction
    \/ RecipientCatchesUpAction
    \/ RecipientFailsMigrationAction
    \/ CloudSendsDonorStartMigrationRequestAction
    \/ CloudSendsDonorForgetMigrationRequestAction
    \/ DonorAbortsIndexBuildsAction
    \/ DonorSendsRecipientSyncDataReturnAfterPinningRequestAction
    \/ DonorSendsRecipientSyncDataRequestAction
    \/ DonorSendsRecipientSyncDataReturnAfterReachingDonorTimestampRequestAction
    \/ ReceiveRecipientSyncDataReturnAfterPinningRequestAction
    \/ ReceiveRecipientSyncDataReturnAfterPinningResponseAction
    \/ ReceiveDonorStartMigrationRequestAction
    \/ ReceiveDonorStartMigrationResponseAction
    \/ ReceiveRecipientSyncDataRequestAction
    \/ ReceiveRecipientSyncDataResponseAction
    \/ ReceiveRecipientSyncDataReturnAfterReachingDonorTimestampRequestAction
    \/ ReceiveRecipientSyncDataReturnAfterReachingDonorTimestampResponseAction
    \/ ReceiveDonorForgetMigrationRequestAction
    \/ ReceiveDonorForgetMigrationResponseAction
    \/ ReceiveRecipientForgetMigrationRequestAction
    \/ ReceiveRecipientForgetMigrationResponseAction

\* Add fairness constraints so the above liveness properties are met.
Liveness ==
    /\ WF_vars(ReceiveDonorStartMigrationRequestAction)
    /\ WF_vars(ReceiveDonorStartMigrationResponseAction)
    /\ WF_vars(ReceiveRecipientSyncDataReturnAfterPinningRequestAction)
    /\ WF_vars(ReceiveRecipientSyncDataReturnAfterPinningResponseAction)
    /\ WF_vars(ReceiveRecipientSyncDataRequestAction)
    /\ WF_vars(ReceiveRecipientSyncDataResponseAction)
    /\ WF_vars(ReceiveRecipientSyncDataReturnAfterReachingDonorTimestampRequestAction)
    /\ WF_vars(ReceiveRecipientSyncDataReturnAfterReachingDonorTimestampResponseAction)
    /\ WF_vars(ReceiveDonorForgetMigrationRequestAction)
    /\ WF_vars(ReceiveDonorForgetMigrationResponseAction)
    /\ WF_vars(ReceiveRecipientForgetMigrationRequestAction)
    /\ WF_vars(ReceiveRecipientForgetMigrationResponseAction)
    /\ WF_vars(CloudSendsDonorStartMigrationRequestAction)
    /\ WF_vars(CloudSendsDonorForgetMigrationRequestAction)

Spec == Init /\ [][Next]_vars /\ Liveness

=============================================================================
