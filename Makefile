mongotop-kerberos:
	go build -o mongotop -tags sasl src/github.com/mongodb/mongo-tools/mongotop/main/main.go

mongotop:
	go build -o mongotop src/github.com/mongodb/mongo-tools/mongotop/main/main.go
