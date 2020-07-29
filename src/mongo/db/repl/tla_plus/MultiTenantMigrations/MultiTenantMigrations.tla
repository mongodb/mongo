\* Copyright 2020 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

----------------------------- MODULE MultiTenantMigrations -----------------------------
\*
\* A specification of MongoDB's multi-tenant migrations state-machine protocol.
\*
\* To run the model-checker, first edit the constants in MCMultiTenantMigrations.cfg if desired,
\* then:
\*     cd src/mongo/db/repl/tla_plus
\*     ./model-check.sh MultiTenantMigrations
\*

EXTENDS Integers, FiniteSets, Sequences, TLC

\* Command requests and responses
CONSTANTS DonorStartMigrationRequest, DonorStartMigrationResponse
CONSTANTS RecipientSyncData1Request, RecipientSyncData1Response
CONSTANTS RecipientSyncData2Request, RecipientSyncData2Response
CONSTANTS DonorForgetMigrationRequest, DonorForgetMigrationResponse
CONSTANTS RecipientForgetMigrationRequest, RecipientForgetMigrationResponse

\* recipient states
\* RecUnstarted -(syncData1)-> RecInconsistent -> RecConsistent -(syncData2)-> RecLagged -> RecReady
CONSTANTS RecUnstarted, RecInconsistent, RecConsistent, RecLagged, RecReady, RecAborted,
    RecTerminalState

\* donor states
\* DonUnstarted -(startMigration)-> DonDataSync -(syncData1Res)
\*      -> DonBlocking -(syncData2Res)-> DonCommitted
CONSTANTS DonUnstarted, DonDataSync, DonBlocking, DonCommitted, DonAborted, DonTerminalState

\* cloud state
CONSTANTS CloudUnknown, CloudCommitted, CloudAborted, CloudTerminalState

\* Responses to DonorStartMigration request
CONSTANTS MigrationNone, MigrationCommitted, MigrationAborted

\* Responses to RecipientSyncData1/2 requests
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
   m.mType \in {DonorStartMigrationRequest, RecipientSyncData1Request,
            RecipientSyncData2Request, DonorForgetMigrationRequest,
            RecipientForgetMigrationRequest}

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
      [] donorState \in {DonUnstarted, DonDataSync, DonBlocking, DonTerminalState} ->
         [mType    |-> DonorStartMigrationResponse,
          mOutcome |-> MigrationNone]

\* Donor
HandleDonorStartMigrationRequest(m) ==
    /\ m.mType = DonorStartMigrationRequest
    \* If the donor is unstarted, it starts, otherwise nothing happens. Either way sends a response
    \* to cloud.
    /\ CASE donorState = DonUnstarted ->
            /\ donorState' = DonDataSync
            /\ messages' = WithoutMessage(m,
                           \* Send an immediate response to cloud.
                           WithMessage(DonorStartMigrationResponseGen,
                           \* Send a recipientSyncData1 request to the recipient.
                           WithMessage([mType |-> RecipientSyncData1Request], messages)))
            /\ totalRequests' = totalRequests + 1
            /\ totalResponses' = totalResponses + 1
         [] donorState \in {DonDataSync, DonBlocking, DonCommitted, DonAborted, DonTerminalState} ->
            /\ SendAndDiscard(DonorStartMigrationResponseGen, m)
            /\ UNCHANGED <<donorVars>>
    /\ UNCHANGED <<recipientVars, cloudVars>>

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

\* Recipient
HandleRecipientSyncData1Request(m) ==
    /\ m.mType = RecipientSyncData1Request
    \* We don't want to handle this request being processed while inconsistent, since that would
    \* require modeling request joining behavior, which is unnecessary complexity for the
    \* purposes of this model. A recipientSyncData1 request being processed while in RecInconsistent
    \* must be a duplicate message.
    /\ recipientState \notin {RecInconsistent}
    /\ CASE recipientState = RecUnstarted ->
            \* Starts the migration. The recipient does not respond to the donor until it is
            \* consistent.
            /\ recipientState' = RecInconsistent
            /\ Discard(m)
            /\ UNCHANGED <<recipientAborted>>
         [] recipientState = RecAborted ->
            \* Sends a response to the donor that the migration aborted.
            /\ SendAndDiscard([mType |-> RecipientSyncData1Response,
                               mSyncStatus |-> SyncAborted], m)
            /\ UNCHANGED <<recipientVars>>
         [] recipientState \in {RecConsistent, RecLagged, RecReady} ->
            \* This is a duplicate message, resend the response we must have sent previously.
            /\ SendAndDiscard([mType |-> RecipientSyncData1Response,
                               mSyncStatus |-> SyncOK], m)
            /\ UNCHANGED <<recipientVars>>
         [] recipientState = RecTerminalState ->
            \* This migration has finished, which means the donor and cloud already have committed
            \* or aborted. Send SyncOK, which will be ignored.
            /\ SendAndDiscard([mType |-> RecipientSyncData1Response,
                               mSyncStatus |-> SyncOK], m)
            /\ UNCHANGED <<recipientVars>>
    /\ UNCHANGED <<donorVars, cloudVars>>

\* Factored out of below to make nested Case statements clearer.
HandleRecipientSyncData1Response_SyncOK(m) ==
    CASE donorState = DonDataSync ->
         \* Move the state machine to "blocking" and send recipientSyncData2.
         /\ donorState' = DonBlocking
         /\ SendAndDiscard([mType |-> RecipientSyncData2Request], m)
      [] donorState \in {DonBlocking, DonCommitted, DonAborted, DonTerminalState} ->
         \* Just ignore this message, since we're past this step in the protocol
         \* and this is a delayed message.
         /\ Discard(m)
         /\ UNCHANGED <<donorState>>

\* Factored out of below to make nested Case statements clearer.
HandleRecipientSyncData1Response_SyncAborted(m) ==
    /\ CASE donorState = DonDataSync ->
            \* The recipient failed the migration, so abort.
            \* We can only get this response in DonDataSync or DonBlocking.
            \* DataSync is the common case, but Blocking can happen when there are two
            \* recipientSyncData1 responses and the "OK" one is processed first.
            donorState' = DonAborted
         [] donorState \in {DonBlocking, DonAborted, DonTerminalState} ->
            \* If the migration is in DonBlocking, we ignore the response. The migration will be
            \* aborted on the donor when it receives the recipientSyncData2 response.
            \* If the migration is already aborted or finished, do nothing.
            UNCHANGED <<donorState>>
    /\ Discard(m)

\* Donor
HandleRecipientSyncData1Response(m) ==
    /\ m.mType = RecipientSyncData1Response
    /\ CASE m.mSyncStatus = SyncOK ->
            HandleRecipientSyncData1Response_SyncOK(m)
         [] m.mSyncStatus = SyncAborted ->
            HandleRecipientSyncData1Response_SyncAborted(m)
    /\ UNCHANGED <<recipientVars, cloudVars>>

\* Recipient
HandleRecipientSyncData2Request(m) ==
    /\ m.mType = RecipientSyncData2Request
    \* We don't want to handle this request being processed while lagged, since that would
    \* require modeling request joining behavior, which is unnecessary complexity for the
    \* purposes of this model. A recipientSyncData2 request being processed while in RecLagged
    \* must be a duplicate message.
    /\ recipientState \notin {RecLagged}
    /\ CASE recipientState = RecConsistent ->
            \* Move the state machine to "lagged", since the recipient now knows the ending
            \* timestamp. The recipient does not respond to the donor until it has caught up.
            /\ recipientState' = RecLagged
            /\ Discard(m)
            /\ UNCHANGED <<recipientAborted>>
         [] recipientState = RecAborted ->
            \* Sends a response to the donor that the migration aborted.
            /\ SendAndDiscard([mType |-> RecipientSyncData2Response,
                               mSyncStatus |-> SyncAborted], m)
            /\ UNCHANGED <<recipientVars>>
         [] recipientState = RecReady ->
            \* This is a duplicate message, resend the response we must have sent previously.
            /\ SendAndDiscard([mType |-> RecipientSyncData2Response,
                                mSyncStatus |-> SyncOK], m)
            /\ UNCHANGED <<recipientVars>>
         [] recipientState = RecTerminalState ->
            \* This migration is finished, which means the donor and cloud already have committed
            \* or aborted. Send SyncOK, which will be ignored.
            /\ SendAndDiscard([mType |-> RecipientSyncData2Response,
                                mSyncStatus |-> SyncOK], m)
            /\ UNCHANGED <<recipientVars>>
    /\ UNCHANGED <<donorVars, cloudVars>>

\* Factored out of below to make nested Case statements clearer.
HandleRecipientSyncData2Response_SyncOK ==
    CASE donorState = DonBlocking ->
         \* The recipient is done!
         donorState' = DonCommitted
      [] donorState \in {DonCommitted, DonAborted, DonTerminalState} ->
         \* Just ignore this message, since we're past this step in the protocol
         \* and this is a delayed message.
         UNCHANGED <<donorState>>

\* Factored out of below to make nested Case statements clearer.
HandleRecipientSyncData2Response_SyncAborted ==
    CASE donorState = DonBlocking ->
         \*  The recipient failed the migration, so abort.
         donorState' = DonAborted
      [] donorState \in {DonAborted, DonTerminalState} ->
         \* If the migration is already aborted or finished, do nothing.
         UNCHANGED <<donorState>>
\* Donor
HandleRecipientSyncData2Response(m) ==
    /\ m.mType = RecipientSyncData2Response
    /\ CASE m.mSyncStatus = SyncOK ->
            HandleRecipientSyncData2Response_SyncOK
         [] m.mSyncStatus = SyncAborted ->
            HandleRecipientSyncData2Response_SyncAborted
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
    /\ cloudState' = CloudTerminalState
    /\ Discard(m)
    /\ UNCHANGED <<donorVars, recipientVars>>

\* Recipient
HandleRecipientForgetMigrationRequest(m) ==
    /\ m.mType = RecipientForgetMigrationRequest
    \* Finish the migration no matter what, and tell the donor.
    /\ recipientState' = RecTerminalState
    /\ SendAndDiscard([mType |-> RecipientForgetMigrationResponse], m)
    /\ UNCHANGED <<donorVars, cloudVars, recipientAborted>>

\* Donor
HandleRecipientForgetMigrationResponse(m) ==
    /\ m.mType = RecipientForgetMigrationResponse
    \* The recipient has finished the migration, so now the donor can finish the migration and
    \* respond to cloud that it has finished the migration.
    /\ donorState' = DonTerminalState
    /\ SendAndDiscard([mType |-> DonorForgetMigrationResponse], m)
    /\ UNCHANGED <<recipientVars, cloudVars>>


(******************************************************************************)
(* [ACTION]                                                                   *)
(******************************************************************************)

\* Models a retry of recipientSyncData1.
DonorSendsRecipientSyncData1Request ==
    /\ donorState = DonDataSync
    /\ Send([mType |-> RecipientSyncData1Request])
    /\ UNCHANGED <<donorVars, recipientVars, cloudVars>>

\* Models a retry of recipientSyncData2.
DonorSendsRecipientSyncData2Request ==
    /\ donorState = DonBlocking
    /\ Send([mType |-> RecipientSyncData2Request])
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
    /\ recipientState = RecInconsistent
    /\ recipientState' = RecConsistent
    /\ Send([mType |-> RecipientSyncData1Response,
             mSyncStatus |-> SyncOK])
    /\ UNCHANGED <<donorVars, cloudVars, recipientAborted>>

RecipientCatchesUp ==
    /\ recipientState = RecLagged
    /\ recipientState' = RecReady
    /\ Send([mType |-> RecipientSyncData2Response,
             mSyncStatus |-> SyncOK])
    /\ UNCHANGED <<donorVars, cloudVars, recipientAborted>>

RecipientFailsMigration ==
    \* Recipient can't fail after it's ready, finished, or already aborted.
    /\ recipientState \notin {RecReady, RecAborted, RecTerminalState}
    /\ recipientState' = RecAborted
    /\ recipientAborted' = TRUE
    /\ CASE recipientState = RecInconsistent ->
            \* When "inconsistent" the recipient has an active recipientSyncData1 request.
            Send([mType |-> RecipientSyncData1Response,
                    mSyncStatus |-> SyncAborted])
         [] recipientState = RecLagged ->
            \* When "lagged" the recipient has an active recipientSyncData2 request.
            Send([mType |-> RecipientSyncData2Response,
                  mSyncStatus |-> SyncAborted])
         [] recipientState \in {RecUnstarted, RecConsistent} ->
            \* Nothing happens, besides setting the state to aborted. On transitioning to
            \* "consistent" the recipient already sent a response to the donor and should not
            \* send a conflicting response.
            UNCHANGED <<messageVars>>
    /\ UNCHANGED <<cloudVars, donorVars>>

(**************************************************************************************************)
(* Correctness Properties                                                                         *)
(**************************************************************************************************)

StateMachinesInconsistent ==
    \/ /\ cloudState = CloudCommitted
       /\ \/ recipientState \notin {RecReady, RecTerminalState}
          \/ recipientAborted = TRUE
          \/ donorState \notin {DonCommitted, DonTerminalState}
    \/ /\ donorState = DonCommitted
       /\ \/ recipientState \notin {RecReady, RecTerminalState}
          \/ recipientAborted = TRUE

StateMachinesConsistent == ~StateMachinesInconsistent

(**************************************************************************************************)
(* Liveness properties                                                                            *)
(**************************************************************************************************)

\* Checks that the state machines eventually converge on terminating states.
MigrationEventuallyCompletes ==
    <> /\ recipientState = RecTerminalState
       /\ donorState = DonTerminalState
       /\ cloudState = CloudTerminalState

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
    /\ donorState = DonUnstarted
    /\ recipientState = RecUnstarted
    /\ cloudState = CloudUnknown
    /\ totalRequests = 0
    /\ totalResponses = 0
    /\ recipientAborted = FALSE

RecipientBecomesConsistentAction == RecipientBecomesConsistent
RecipientCatchesUpAction == RecipientCatchesUp
RecipientFailsMigrationAction == RecipientFailsMigration
CloudSendsDonorStartMigrationRequestAction == CloudSendsDonorStartMigrationRequest
CloudSendsDonorForgetMigrationRequestAction == CloudSendsDonorForgetMigrationRequest
DonorSendsRecipientSyncData1RequestAction == DonorSendsRecipientSyncData1Request
DonorSendsRecipientSyncData2RequestAction == DonorSendsRecipientSyncData2Request

ReceiveDonorStartMigrationRequestAction == \E m \in DOMAIN messages :
    HandleDonorStartMigrationRequest(m)
ReceiveDonorStartMigrationResponseAction == \E m \in DOMAIN messages :
    HandleDonorStartMigrationResponse(m)
ReceiveRecipientSyncData1RequestAction == \E m \in DOMAIN messages :
    HandleRecipientSyncData1Request(m)
ReceiveRecipientSyncData1ResponseAction == \E m \in DOMAIN messages :
    HandleRecipientSyncData1Response(m)
ReceiveRecipientSyncData2RequestAction == \E m \in DOMAIN messages :
    HandleRecipientSyncData2Request(m)
ReceiveRecipientSyncData2ResponseAction == \E m \in DOMAIN messages :
    HandleRecipientSyncData2Response(m)
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
    \/ DonorSendsRecipientSyncData1RequestAction
    \/ DonorSendsRecipientSyncData2RequestAction
    \/ ReceiveDonorStartMigrationRequestAction
    \/ ReceiveDonorStartMigrationResponseAction
    \/ ReceiveRecipientSyncData1RequestAction
    \/ ReceiveRecipientSyncData1ResponseAction
    \/ ReceiveRecipientSyncData2RequestAction
    \/ ReceiveRecipientSyncData2ResponseAction
    \/ ReceiveDonorForgetMigrationRequestAction
    \/ ReceiveDonorForgetMigrationResponseAction
    \/ ReceiveRecipientForgetMigrationRequestAction
    \/ ReceiveRecipientForgetMigrationResponseAction

\* Add fairness constraints so the above liveness properties are met.
Liveness ==
    /\ WF_vars(ReceiveDonorStartMigrationRequestAction)
    /\ WF_vars(ReceiveDonorStartMigrationResponseAction)
    /\ WF_vars(ReceiveRecipientSyncData1RequestAction)
    /\ WF_vars(ReceiveRecipientSyncData1ResponseAction)
    /\ WF_vars(ReceiveRecipientSyncData2RequestAction)
    /\ WF_vars(ReceiveRecipientSyncData2ResponseAction)
    /\ WF_vars(ReceiveDonorForgetMigrationRequestAction)
    /\ WF_vars(ReceiveDonorForgetMigrationResponseAction)
    /\ WF_vars(ReceiveRecipientForgetMigrationRequestAction)
    /\ WF_vars(ReceiveRecipientForgetMigrationResponseAction)
    /\ WF_vars(CloudSendsDonorStartMigrationRequestAction)
    /\ WF_vars(CloudSendsDonorForgetMigrationRequestAction)

Spec == Init /\ [][Next]_vars /\ Liveness

=============================================================================
