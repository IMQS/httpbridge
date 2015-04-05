package main

import (
	"fmt"
	httpbridge "github.com/benharper123/httpbridge/go"
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
