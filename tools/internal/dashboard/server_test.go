package dashboard

import (
	"context"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"testing"
	"time"

	"github.com/dongwonkwak/dbgate/tools/internal/client"
)

func TestNewServer_Routes(t *testing.T) {
	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	c := client.NewClient("/tmp/nonexistent.sock", time.Second)
	srv := NewServer(":0", c, logger, "", "")

	if srv.mux == nil {
		t.Fatal("mux should not be nil")
	}
	if srv.chart == nil {
		t.Fatal("chart buffer should not be nil")
	}
}

func TestBasicAuthMiddleware(t *testing.T) {
	inner := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	})

	protected := basicAuthMiddleware("admin", "secret", inner)

	tests := []struct {
		name     string
		user     string
		pass     string
		wantCode int
	}{
		{"valid credentials", "admin", "secret", http.StatusOK},
		{"wrong password", "admin", "wrong", http.StatusUnauthorized},
		{"wrong user", "other", "secret", http.StatusUnauthorized},
		{"empty credentials", "", "", http.StatusUnauthorized},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			req := httptest.NewRequest(http.MethodGet, "/", http.NoBody)
			if tt.user != "" || tt.pass != "" {
				req.SetBasicAuth(tt.user, tt.pass)
			}
			rec := httptest.NewRecorder()
			protected.ServeHTTP(rec, req)
			if rec.Code != tt.wantCode {
				t.Errorf("expected %d, got %d", tt.wantCode, rec.Code)
			}
		})
	}
}

func TestServer_Run_GracefulShutdown(t *testing.T) {
	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	c := client.NewClient("/tmp/nonexistent.sock", time.Second)
	srv := NewServer(":0", c, logger, "", "")

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
