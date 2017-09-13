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
