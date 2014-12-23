package util

import (
    "time"
)

var (
    acceptedDateFormats = []string{
        "2006-01-02T15:04:05.000Z0700",
        "2006-01-02T15:04:05Z0700",
        "2006-01-02T15:04Z0700",
    }
)

func FormatDate(v string)(interface{}, error) {
    var date interface{}
    var err error

    for _, format := range acceptedDateFormats {
        date, err := time.Parse(format, v)
        if err == nil {
            return date, nil
        }
    }
    return date, err
}
