mongotop-kerberos:
	go build -o mongotop -tags sasl src/github.com/shelman/mongo-tools-proto/mongotop/main/main.go

mongotop:
	go build -o mongotop src/github.com/shelman/mongo-tools-proto/mongotop/main/main.go
