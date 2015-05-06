package main

import (
	"fmt"
	"httpbridge"
	//httpbridge "github.com/bmharper/httpbridge/go/src/httpbridge"  --  How to include httpbridge from a regular project
)

func main() {
	server := httpbridge.Server{}
	server.HttpPort = ":8080"
	server.BackendPort = ":8081"
	err := server.ListenAndServe()
	if err != nil {
		fmt.Printf("Error: %v\n", err)
	}
}
