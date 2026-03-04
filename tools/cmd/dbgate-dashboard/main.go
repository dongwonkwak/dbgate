// Command dbgate-dashboard serves a web dashboard for real-time dbgate proxy
// monitoring. It communicates with the C++ dbgate core via Unix Domain Socket
// and renders statistics using htmx for live updates.
//
// Usage:
//
//	dbgate-dashboard [--listen :8081] [--socket /tmp/dbgate.sock] [--timeout 5s]
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
	listen := flag.String("listen", ":8081", "HTTP listen address")
	socket := flag.String("socket", "/tmp/dbgate.sock", "Path to dbgate Unix Domain Socket")
	timeout := flag.Duration("timeout", 5*time.Second, "Timeout for UDS requests")
	flag.Parse()

	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{
		Level: slog.LevelInfo,
	}))

	c := client.NewClient(*socket, *timeout)
	srv := dashboard.NewServer(*listen, c, logger)

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	return srv.Run(ctx)
}
