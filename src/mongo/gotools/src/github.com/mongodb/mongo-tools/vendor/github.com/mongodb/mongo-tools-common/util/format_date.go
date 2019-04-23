// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package util

import (
    "time"
)

var (
    acceptedDateFormats = []string{
        "2006-01-02T15:04:05.000Z",
        "2006-01-02T15:04:05Z",
        "2006-01-02T15:04Z",
        "2006-01-02T15:04:05.000-0700",
        "2006-01-02T15:04:05-0700",
        "2006-01-02T15:04-0700",
        "2006-01-02T15:04:05Z07:00",
    }
)

func FormatDate(v string)(interface{}, error) {
    var date interface{}
    var err error

    for _, format := range acceptedDateFormats {
        date, err = time.Parse(format, v)
        if err == nil {
            return date, nil
        }
    }
    return date, err
}
