/* This is a tiny httpbridge server that was created in order to serve
unit tests for C++ HTTP APIs.
*/
package main

import (
	"fmt"
	"httpbridge"
	"net/http"
	"os"
	// "github.com/IMQS/httpbridge/go/src/httpbridge"  --  How to include httpbridge from a regular project
)

func showHelp() {
	fmt.Printf("unit-test-server <http port> <httpbridge port>\n")
}

func main() {
	if len(os.Args) != 3 {
		showHelp()
		os.Exit(1)
	}

	httpPort := "127.0.0.1:" + os.Args[1]
	hbPort := "127.0.0.1:" + os.Args[2]

	hb := httpbridge.Server{}
	hb.DisableHttpListener = true
	hb.BackendPort = hbPort
	hb.Log.Level = httpbridge.LogLevelInfo
	go func() {
		err := hb.ListenAndServe()
		if err != nil {
			fmt.Printf("Error: %v\n", err)
		}
	}()
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		// Pass request onto httpbridge
		hb.ServeHTTP(w, r)

		// Listen for special stop signal
		if r.URL.Path == "/stop" {
			fmt.Printf("Received stop signal. Exiting\n")
			os.Exit(0)
		}
	})
	http.ListenAndServe(httpPort, nil)
	os.Exit(0)
}
