// Package dashboard provides an HTTP server for the dbgate web dashboard.
//
// It renders real-time proxy statistics (QPS, block rate, sessions) using
// htmx for partial page updates and Pico CSS for styling. All static assets
// are embedded via Go's embed package for air-gapped deployments.
package dashboard

import (
	"context"
	"crypto/subtle"
	"errors"
	"io/fs"
	"log/slog"
	"net/http"
	"time"

	"github.com/dongwonkwak/dbgate/tools/internal/client"
)

// Server is the HTTP server for the dbgate web dashboard.
type Server struct {
	listenAddr   string
	client       *client.Client
	logger       *slog.Logger
	mux          *http.ServeMux
	chart        *chartBuffer
	authUser     string
	authPassword string
}

// NewServer creates a new dashboard Server.
// authUser and authPassword enable HTTP Basic Auth when both are non-empty.
func NewServer(listenAddr string, c *client.Client, logger *slog.Logger, authUser, authPassword string) *Server {
	s := &Server{
		listenAddr:   listenAddr,
		client:       c,
		logger:       logger,
		mux:          http.NewServeMux(),
		chart:        newChartBuffer(60),
		authUser:     authUser,
		authPassword: authPassword,
	}
	s.registerRoutes()
	return s
}

// basicAuthMiddleware wraps h with HTTP Basic Auth using constant-time comparison.
func basicAuthMiddleware(user, password string, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		u, p, ok := r.BasicAuth()
		uMatch := subtle.ConstantTimeCompare([]byte(u), []byte(user))
		pMatch := subtle.ConstantTimeCompare([]byte(p), []byte(password))
		if !ok || uMatch != 1 || pMatch != 1 {
			w.Header().Set("WWW-Authenticate", `Basic realm="dbgate dashboard"`)
			http.Error(w, "Unauthorized", http.StatusUnauthorized)
			return
		}
		next.ServeHTTP(w, r)
	})
}

// registerRoutes sets up all HTTP routes.
func (s *Server) registerRoutes() {
	s.mux.HandleFunc("GET /", s.handleIndex)
	s.mux.HandleFunc("GET /api/stats", s.handleStats)
	s.mux.HandleFunc("GET /api/sessions", s.handleSessions)
	s.mux.HandleFunc("GET /api/chart-data", s.handleChartData)
	s.mux.HandleFunc("GET /api/policy-versions", s.handlePolicyVersions)
	s.mux.HandleFunc("POST /api/policy-rollback", s.handlePolicyRollback)
	s.mux.HandleFunc("GET /policy-tester", s.handlePolicyTester)
	s.mux.HandleFunc("POST /api/policy-explain", s.handlePolicyExplain)
	staticSub, err := fs.Sub(staticFS, "static")
	if err != nil {
		panic("embedded static FS: " + err.Error())
	}
	s.mux.Handle("GET /static/", http.StripPrefix("/static/", http.FileServerFS(staticSub)))
}

// Run starts the HTTP server and blocks until ctx is cancelled.
// It performs a graceful shutdown with a 5-second deadline.
func (s *Server) Run(ctx context.Context) error {
	var handler http.Handler = s.mux
	if s.authUser != "" {
		handler = basicAuthMiddleware(s.authUser, s.authPassword, handler)
	}
	srv := &http.Server{
		Addr:              s.listenAddr,
		Handler:           handler,
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
