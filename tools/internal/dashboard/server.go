// Package dashboard provides an HTTP server for the dbgate web dashboard.
//
// It renders real-time proxy statistics (QPS, block rate, sessions) using
// htmx for partial page updates and Pico CSS for styling. All static assets
// are embedded via Go's embed package for air-gapped deployments.
package dashboard

import (
	"context"
	"errors"
	"io/fs"
	"log/slog"
	"net/http"
	"time"

	"github.com/dongwonkwak/dbgate/tools/internal/client"
)

// Server is the HTTP server for the dbgate web dashboard.
type Server struct {
	listenAddr string
	client     *client.Client
	logger     *slog.Logger
	mux        *http.ServeMux
	chart      *chartBuffer
}

// NewServer creates a new dashboard Server.
func NewServer(listenAddr string, c *client.Client, logger *slog.Logger) *Server {
	s := &Server{
		listenAddr: listenAddr,
		client:     c,
		logger:     logger,
		mux:        http.NewServeMux(),
		chart:      newChartBuffer(60),
	}
	s.registerRoutes()
	return s
}

// registerRoutes sets up all HTTP routes.
func (s *Server) registerRoutes() {
	s.mux.HandleFunc("GET /", s.handleIndex)
	s.mux.HandleFunc("GET /api/stats", s.handleStats)
	s.mux.HandleFunc("GET /api/sessions", s.handleSessions)
	s.mux.HandleFunc("GET /api/chart-data", s.handleChartData)
	staticSub, err := fs.Sub(staticFS, "static")
	if err != nil {
		panic("embedded static FS: " + err.Error())
	}
	s.mux.Handle("GET /static/", http.StripPrefix("/static/", http.FileServerFS(staticSub)))
}

// Run starts the HTTP server and blocks until ctx is cancelled.
// It performs a graceful shutdown with a 5-second deadline.
func (s *Server) Run(ctx context.Context) error {
	srv := &http.Server{
		Addr:              s.listenAddr,
		Handler:           s.mux,
		ReadHeaderTimeout: 10 * time.Second,
	}

	errCh := make(chan error, 1)
	go func() {
		s.logger.Info("dashboard server starting", "addr", s.listenAddr)
		if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			errCh <- err
		}
		close(errCh)
	}()

	select {
	case err := <-errCh:
		return err
	case <-ctx.Done():
		s.logger.Info("shutting down dashboard server")
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		return srv.Shutdown(shutdownCtx)
	}
}
