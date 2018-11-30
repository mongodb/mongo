// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package dateconv

import (
	"strings"
)

var (
	// msReplacers based on:
	// https://msdn.microsoft.com/en-us/library/ee634398(v=sql.130).aspx
	msReplacers = []string{
		"dddd", "Monday",
		"ddd", "Mon",
		"dd", "02",
		"d", "2",
		"MMMM", "January",
		"MMM", "Jan",
		"MM", "01",
		"M", "1",
		// "gg", "?",
		"hh", "03",
		"h", "3",
		"HH", "15",
		"H", "15",
		"mm", "04",
		"m", "4",
		"ss", "05",
		"s", "5",
		// "f", "?",
		"tt", "PM",
		// "t", "?",
		"yyyy", "2006",
		"yyy", "2006",
		"yy", "06",
		// "y", "?",
		"zzz", "-07:00",
		"zz", "-07",
		// "z", "?",
	}
	msStringReplacer = strings.NewReplacer(msReplacers...)
)

// FromMS reformats a datetime layout string from the Microsoft SQL Server
// FORMAT function into go's parse format.
func FromMS(layout string) string {
	return msStringReplacer.Replace(layout)
}

var (
	// oracleReplacers based on:
	// http://docs.oracle.com/cd/B19306_01/server.102/b14200/sql_elements004.htm#i34924
	oracleReplacers = []string{
		"AM", "PM",
		"DAY", "Monday",
		"DY", "Mon",
		"DD", "02",
		"HH12", "03",
		"HH24", "15",
		"HH", "03",
		"MI", "04",
		"MONTH", "January",
		"MON", "Jan",
		"MM", "01",
		"SS", "05",
		"TZD", "MST",
		"TZH:TZM", "-07:00",
		"TZH", "-07",
		"YYYY", "2006",
		"YY", "06",
	}
	oracleStringReplacer = strings.NewReplacer(oracleReplacers...)
)

// FromOrace reformats a datetime layout string from the Oracle Database
// TO_DATE function into go's parse format.
func FromOracle(layout string) string {
	return oracleStringReplacer.Replace(strings.ToUpper(layout))
}
