package main

import (
	"fmt"
	httpbridge "github.com/benharper123/httpbridge/go"
)

func main() {
	server := httpbridge.Server{}
	err := server.ListenAndServe()
	if err != nil {
		fmt.Printf("Error: %v\n", err)
	}
}
