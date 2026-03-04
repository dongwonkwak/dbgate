// Command dbgate-dashboard serves a web dashboard for real-time dbgate proxy
// monitoring. It communicates with the C++ dbgate core via Unix Domain Socket
// and renders statistics using htmx for live updates.
//
// Basic Auth is mandatory. Set credentials via flags or environment variables:
//
//	dbgate-dashboard --listen 127.0.0.1:8081 --socket /tmp/dbgate.sock \
//	                 --auth-user USER --auth-password PASS
//
// Alternatively, set DASHBOARD_AUTH_USER and DASHBOARD_AUTH_PASSWORD env vars.
// The server refuses to start if either credential is missing.
package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/dongwonkwak/dbgate/tools/internal/client"
	"github.com/dongwonkwak/dbgate/tools/internal/dashboard"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}
}

func run() error {
	listen := flag.String("listen", "127.0.0.1:8081", "HTTP listen address")
	socket := flag.String("socket", "/tmp/dbgate.sock", "Path to dbgate Unix Domain Socket")
	timeout := flag.Duration("timeout", 5*time.Second, "Timeout for UDS requests")
	authUser := flag.String("auth-user", "", "Basic auth username (env: DASHBOARD_AUTH_USER)")
	authPassword := flag.String("auth-password", "", "Basic auth password (env: DASHBOARD_AUTH_PASSWORD)")
	flag.Parse()

	if *authUser == "" {
		*authUser = os.Getenv("DASHBOARD_AUTH_USER")
	}
	if *authPassword == "" {
		*authPassword = os.Getenv("DASHBOARD_AUTH_PASSWORD")
	}
	if *authUser == "" || *authPassword == "" {
		return fmt.Errorf("dashboard auth is required: set --auth-user/--auth-password or DASHBOARD_AUTH_USER/DASHBOARD_AUTH_PASSWORD")
	}

	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{
		Level: slog.LevelInfo,
	}))

	c := client.NewClient(*socket, *timeout)
	srv := dashboard.NewServer(*listen, c, logger, *authUser, *authPassword)

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	return srv.Run(ctx)
}
