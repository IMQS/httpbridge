package main

import (
	"fmt"
	"httpbridge"
	// "github.com/IMQS/httpbridge/go/src/httpbridge"  --  How to include httpbridge from a regular project
)

func main() {
	server := httpbridge.Server{}
	server.HttpPort = "127.0.0.1:8080"
	server.BackendPort = "127.0.0.1:8081"
	server.Log.Level = httpbridge.LogLevelDebug
	err := server.ListenAndServe()
	if err != nil {
		fmt.Printf("Error: %v\n", err)
	}
}
