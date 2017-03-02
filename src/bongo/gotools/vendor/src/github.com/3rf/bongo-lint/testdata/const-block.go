// Test for docs in const blocks

// Package foo ...
package foo

const (
	// Prefix for something.
	// MATCH /InlineWhatever.*form/
	InlineWhatever = "blah"

	Whatsit = "missing_comment" // MATCH /Whatsit.*should have comment.*block/

	// We should only warn once per block for missing comments,
	// but always complain about malformed comments.

	WhosYourDaddy = "another_missing_one"

	// Something
	// MATCH /WhatDoesHeDo.*form/
	WhatDoesHeDo = "it's not a tumor!"
)

// These shouldn't need doc comments.
const (
	Alpha = "a"
	Beta  = "b"
	Gamma = "g"
)

// The comment on the previous const block shouldn't flow through to here.

const UndocAgain = 6 // MATCH /UndocAgain.*should have comment/

const (
	SomeUndocumented = 7 // MATCH /SomeUndocumented.*should have comment.*block/
)
