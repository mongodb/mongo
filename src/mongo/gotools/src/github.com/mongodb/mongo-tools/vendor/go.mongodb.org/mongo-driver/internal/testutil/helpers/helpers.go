// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package testhelpers // import "go.mongodb.org/mongo-driver/internal/testutil/helpers"

import (
	"fmt"
	"io/ioutil"
	"math"
	"path"
	"strings"
	"time"

	"testing"

	"io"

	"reflect"

	"github.com/stretchr/testify/require"
	"go.mongodb.org/mongo-driver/x/mongo/driver/connstring"
)

// Test helpers

// IsNil returns true if the object is nil
func IsNil(object interface{}) bool {
	if object == nil {
		return true
	}

	value := reflect.ValueOf(object)
	kind := value.Kind()

	// checking to see if type is Chan, Func, Interface, Map, Ptr, or Slice
	if kind >= reflect.Chan && kind <= reflect.Slice && value.IsNil() {
		return true
	}

	return false
}

// RequireNotNil throws an error if var is nil
func RequireNotNil(t *testing.T, variable interface{}, msgFormat string, msgVars ...interface{}) {
	if IsNil(variable) {
		t.Fatalf(msgFormat, msgVars...)
	}
}

// RequireNil throws an error if var is not nil
func RequireNil(t *testing.T, variable interface{}, msgFormat string, msgVars ...interface{}) {
	t.Helper()
	if !IsNil(variable) {
		t.Fatalf(msgFormat, msgVars...)
	}
}

// FindJSONFilesInDir finds the JSON files in a directory.
func FindJSONFilesInDir(t *testing.T, dir string) []string {
	files := make([]string, 0)

	entries, err := ioutil.ReadDir(dir)
	require.NoError(t, err)

	for _, entry := range entries {
		if entry.IsDir() || path.Ext(entry.Name()) != ".json" {
			continue
		}

		files = append(files, entry.Name())
	}

	return files
}

// RequireNoErrorOnClose ensures there is not an error when calling Close.
func RequireNoErrorOnClose(t *testing.T, c io.Closer) {
	require.NoError(t, c.Close())
}

// VerifyConnStringOptions verifies the options on the connection string.
func VerifyConnStringOptions(t *testing.T, cs connstring.ConnString, options map[string]interface{}) {
	// Check that all options are present.
	for key, value := range options {

		key = strings.ToLower(key)
		switch key {
		case "appname":
			require.Equal(t, value, cs.AppName)
		case "authsource":
			require.Equal(t, value, cs.AuthSource)
		case "authmechanism":
			require.Equal(t, value, cs.AuthMechanism)
		case "authmechanismproperties":
			convertedMap := value.(map[string]interface{})
			require.Equal(t,
				mapInterfaceToString(convertedMap),
				cs.AuthMechanismProperties)
		case "compressors":
			require.Equal(t, convertToStringSlice(value), cs.Compressors)
		case "connecttimeoutms":
			require.Equal(t, value, float64(cs.ConnectTimeout/time.Millisecond))
		case "heartbeatfrequencyms":
			require.Equal(t, value, float64(cs.HeartbeatInterval/time.Millisecond))
		case "journal":
			require.True(t, cs.JSet)
			require.Equal(t, value, cs.J)
		case "localthresholdms":
			require.True(t, cs.LocalThresholdSet)
			require.Equal(t, value, float64(cs.LocalThreshold/time.Millisecond))
		case "maxidletimems":
			require.Equal(t, value, float64(cs.MaxConnIdleTime/time.Millisecond))
		case "maxpoolsize":
			require.True(t, cs.MaxPoolSizeSet)
			require.Equal(t, value, cs.MaxPoolSize)
		case "maxstalenessseconds":
			require.True(t, cs.MaxStalenessSet)
			require.Equal(t, value, float64(cs.MaxStaleness/time.Second))
		case "minpoolsize":
			require.True(t, cs.MinPoolSizeSet)
			require.Equal(t, value, int64(cs.MinPoolSize))
		case "readpreference":
			require.Equal(t, value, cs.ReadPreference)
		case "readpreferencetags":
			sm, ok := value.([]interface{})
			require.True(t, ok)
			tags := make([]map[string]string, 0, len(sm))
			for _, i := range sm {
				m, ok := i.(map[string]interface{})
				require.True(t, ok)
				tags = append(tags, mapInterfaceToString(m))
			}
			require.Equal(t, tags, cs.ReadPreferenceTagSets)
		case "readconcernlevel":
			require.Equal(t, value, cs.ReadConcernLevel)
		case "replicaset":
			require.Equal(t, value, cs.ReplicaSet)
		case "retrywrites":
			require.True(t, cs.RetryWritesSet)
			require.Equal(t, value, cs.RetryWrites)
		case "serverselectiontimeoutms":
			require.Equal(t, value, float64(cs.ServerSelectionTimeout/time.Millisecond))
		case "ssl", "tls":
			require.Equal(t, value, cs.SSL)
		case "sockettimeoutms":
			require.Equal(t, value, float64(cs.SocketTimeout/time.Millisecond))
		case "tlsallowinvalidcertificates", "tlsallowinvalidhostnames", "tlsinsecure":
			require.True(t, cs.SSLInsecureSet)
			require.Equal(t, value, cs.SSLInsecure)
		case "tlscafile":
			require.True(t, cs.SSLCaFileSet)
			require.Equal(t, value, cs.SSLCaFile)
		case "tlscertificatekeyfile":
			require.True(t, cs.SSLClientCertificateKeyFileSet)
			require.Equal(t, value, cs.SSLClientCertificateKeyFile)
		case "tlscertificatekeyfilepassword":
			require.True(t, cs.SSLClientCertificateKeyPasswordSet)
			require.Equal(t, value, cs.SSLClientCertificateKeyPassword())
		case "w":
			if cs.WNumberSet {
				valueInt := GetIntFromInterface(value)
				require.NotNil(t, valueInt)
				require.Equal(t, *valueInt, int64(cs.WNumber))
			} else {
				require.Equal(t, value, cs.WString)
			}
		case "wtimeoutms":
			require.Equal(t, value, float64(cs.WTimeout/time.Millisecond))
		case "waitqueuetimeoutms":
		case "zlibcompressionlevel":
			require.Equal(t, value, float64(cs.ZlibLevel))
		case "zstdcompressionlevel":
			require.Equal(t, value, float64(cs.ZstdLevel))
		default:
			opt, ok := cs.UnknownOptions[key]
			require.True(t, ok)
			require.Contains(t, opt, fmt.Sprint(value))
		}
	}
}

// Convert each interface{} value in the map to a string.
func mapInterfaceToString(m map[string]interface{}) map[string]string {
	out := make(map[string]string)

	for key, value := range m {
		out[key] = fmt.Sprint(value)
	}

	return out
}

func convertToStringSlice(i interface{}) []string {
	s, ok := i.([]interface{})
	if !ok {
		return nil
	}
	ret := make([]string, 0, len(s))
	for _, v := range s {
		str, ok := v.(string)
		if !ok {
			continue
		}
		ret = append(ret, str)
	}
	return ret
}

// GetIntFromInterface attempts to convert an empty interface value to an integer.
//
// Returns nil if it is not possible.
func GetIntFromInterface(i interface{}) *int64 {
	var out int64

	switch v := i.(type) {
	case int:
		out = int64(v)
	case int32:
		out = int64(v)
	case int64:
		out = v
	case float32:
		f := float64(v)
		if math.Floor(f) != f || f > float64(math.MaxInt64) {
			break
		}

		out = int64(f)

	case float64:
		if math.Floor(v) != v || v > float64(math.MaxInt64) {
			break
		}

		out = int64(v)
	default:
		return nil
	}

	return &out
}
