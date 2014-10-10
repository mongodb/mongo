// +build !ssltest

package mongodump

import (
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	commonOpts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongodump/options"
	"gopkg.in/mgo.v2"
)

func simpleMongoDumpInstance() *MongoDump {
	ssl := &commonOpts.SSL{
		UseSSL: false,
	}
	namespace := &commonOpts.Namespace{
		DB: testDB,
	}
	connection := &commonOpts.Connection{
		Host: testServer,
		Port: testPort,
	}
	toolOptions := &commonOpts.ToolOptions{
		SSL:        ssl,
		Namespace:  namespace,
		Connection: connection,
		Auth:       &commonOpts.Auth{},
		Verbosity:  &commonOpts.Verbosity{},
	}
	outputOptions := &options.OutputOptions{}
	inputOptions := &options.InputOptions{}

	log.SetVerbosity(toolOptions.Verbosity)

	return &MongoDump{
		ToolOptions:   toolOptions,
		InputOptions:  inputOptions,
		OutputOptions: outputOptions,
	}
}

func getBareSession() (*mgo.Session, error) {
	sessionProvider, err := db.InitSessionProvider(commonOpts.ToolOptions{
		Connection: &commonOpts.Connection{
			Host: testServer,
			Port: testPort,
		},
		Auth: &commonOpts.Auth{},
	})
	if err != nil {
		return nil, err
	}
	session, err := sessionProvider.GetSession()
	if err != nil {
		return nil, err
	}
	return session, nil
}
