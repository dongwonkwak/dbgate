package dashboard

import (
	"context"
	"log/slog"
	"os"
	"testing"
	"time"

	"github.com/dongwonkwak/dbgate/tools/internal/client"
)

func TestNewServer_Routes(t *testing.T) {
	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	c := client.NewClient("/tmp/nonexistent.sock", time.Second)
	srv := NewServer(":0", c, logger)

	if srv.mux == nil {
		t.Fatal("mux should not be nil")
	}
	if srv.chart == nil {
		t.Fatal("chart buffer should not be nil")
	}
}

func TestServer_Run_GracefulShutdown(t *testing.T) {
	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	c := client.NewClient("/tmp/nonexistent.sock", time.Second)
	srv := NewServer(":0", c, logger)

	ctx, cancel := context.WithCancel(context.Background())

	errCh := make(chan error, 1)
	go func() {
		errCh <- srv.Run(ctx)
	}()

	// Give server time to start.
	time.Sleep(100 * time.Millisecond)

	// Cancel context to trigger graceful shutdown.
	cancel()

	select {
	case err := <-errCh:
		if err != nil {
			t.Errorf("Run returned unexpected error: %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("server did not shut down within 5 seconds")
	}
}
