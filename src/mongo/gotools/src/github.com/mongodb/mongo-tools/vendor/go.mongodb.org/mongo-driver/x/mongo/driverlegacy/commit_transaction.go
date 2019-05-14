// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package driverlegacy

import (
	"context"
	"time"

	"go.mongodb.org/mongo-driver/mongo/writeconcern"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/topology"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/result"
)

// CommitTransaction handles the full cycle dispatch and execution of committing a transaction
// against the provided topology.
func CommitTransaction(
	ctx context.Context,
	cmd command.CommitTransaction,
	topo *topology.Topology,
	selector description.ServerSelector,
) (result.TransactionResult, error) {
	res, err := commitTransaction(ctx, cmd, topo, selector, nil)

	// Apply majority write concern for retries
	currWC := cmd.Session.CurrentWc
	newTimeout := 10 * time.Second
	if currWC != nil && currWC.GetWTimeout() != 0 {
		newTimeout = currWC.GetWTimeout()
	}
	cmd.Session.CurrentWc = currWC.WithOptions(writeconcern.WMajority(), writeconcern.WTimeout(newTimeout))

	if cerr, ok := err.(command.Error); ok && err != nil {
		// Retry if appropriate
		if cerr.Retryable() {
			res, err = commitTransaction(ctx, cmd, topo, selector, cerr)
			if cerr2, ok := err.(command.Error); ok && err != nil {
				// Retry failures also get label
				cerr2.Labels = append(cerr2.Labels, command.UnknownTransactionCommitResult)
			} else if err != nil {
				err = command.Error{Message: err.Error(), Labels: []string{command.UnknownTransactionCommitResult}}
			}
		}
	}
	return res, err
}

func commitTransaction(
	ctx context.Context,
	cmd command.CommitTransaction,
	topo *topology.Topology,
	selector description.ServerSelector,
	oldErr error,
) (result.TransactionResult, error) {
	if cmd.Session != nil && cmd.Session.PinnedServer != nil {
		selector = cmd.Session.PinnedServer
	}
	ss, err := topo.SelectServer(ctx, selector)
	if err != nil {
		// If retrying server selection, return the original error if it fails
		if oldErr != nil {
			return result.TransactionResult{}, oldErr
		}
		return result.TransactionResult{}, err
	}

	desc := ss.Description()

	if oldErr != nil && (!topo.SupportsSessions() || !description.SessionsSupported(desc.WireVersion)) {
		// Assuming we are retrying (oldErr != nil),
		// if server doesn't support retryable writes, return the original error
		// Conditions for retry write support are the same as that of sessions
		return result.TransactionResult{}, oldErr
	}

	conn, err := ss.Connection(ctx)
	if err != nil {
		if oldErr != nil {
			return result.TransactionResult{}, oldErr
		}
		return result.TransactionResult{}, err
	}
	defer conn.Close()

	res, err := cmd.RoundTrip(ctx, desc, conn)

	// Add UnknownCommitTransaction Error label where appropriate
	if err != nil {
		var newLabels []string
		if cerr, ok := err.(command.Error); ok {
			// Replace the label TransientTransactionError with UnknownTransactionCommitResult
			// if network error, write concern shutdown, or write concern failed errors
			hasUnknownCommitErr := false
			for _, label := range cerr.Labels {
				if label == command.NetworkError {
					hasUnknownCommitErr = true
					break
				}
			}

			// network error, retryable error, or write concern fail/timeout (64) get the unknown label
			if hasUnknownCommitErr || cerr.Retryable() || cerr.Code == 64 {
				for _, label := range cerr.Labels {
					if label != command.TransientTransactionError {
						newLabels = append(newLabels, label)
					}
				}
				newLabels = append(newLabels, command.UnknownTransactionCommitResult)
				cerr.Labels = newLabels
			}
			err = cerr
		}
	}
	return res, err
}
