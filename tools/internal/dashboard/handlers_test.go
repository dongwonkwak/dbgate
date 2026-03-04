package dashboard

import (
	"encoding/binary"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"log/slog"

	"github.com/dongwonkwak/dbgate/tools/internal/client"
)

// startMockUDS starts a mock Unix Domain Socket server that accepts connections,
// drains each request frame, and responds with respJSON (framed).
// It handles multiple connections to support multiple handler calls per test.
func startMockUDS(t *testing.T, respJSON []byte) string {
	t.Helper()

	dir := t.TempDir()
	sockPath := filepath.Join(dir, "mock.sock")

	ln, err := net.Listen("unix", sockPath)
	if err != nil {
		t.Fatalf("listen: %v", err)
	}

	t.Cleanup(func() {
		_ = ln.Close()
		_ = os.Remove(sockPath)
	})

	frame := make([]byte, 4+len(respJSON))
	binary.LittleEndian.PutUint32(frame[:4], uint32(len(respJSON)))
	copy(frame[4:], respJSON)

	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go func(c net.Conn) {
				defer func() { _ = c.Close() }()

				var hdr [4]byte
				if _, err := readFull(c, hdr[:]); err != nil {
					return
				}
				reqLen := binary.LittleEndian.Uint32(hdr[:])
				reqBody := make([]byte, reqLen)
				if _, err := readFull(c, reqBody); err != nil {
					return
				}
				_, _ = c.Write(frame)
			}(conn)
		}
	}()

	return sockPath
}

// readFull reads exactly len(buf) bytes from conn.
func readFull(conn net.Conn, buf []byte) (int, error) {
	total := 0
	for total < len(buf) {
		n, err := conn.Read(buf[total:])
		total += n
		if err != nil {
			return total, err
		}
	}
	return total, nil
}

func newTestServer(t *testing.T, sockPath string) *Server {
	t.Helper()
	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	c := client.NewClient(sockPath, 3*time.Second)
	return NewServer(":0", c, logger, "", "")
}

func makeStatsResponse() []byte {
	resp := map[string]interface{}{
		"ok": true,
		"payload": map[string]interface{}{
			"total_connections": 42,
			"active_sessions":   3,
			"total_queries":     1250,
			"blocked_queries":   15,
			"qps":               25.5,
			"block_rate":        0.012,
			"captured_at_ms":    time.Now().UnixMilli(),
		},
	}
	b, _ := json.Marshal(resp)
	return b
}

func TestHandleIndex_OK(t *testing.T) {
	sockPath := startMockUDS(t, makeStatsResponse())
	srv := newTestServer(t, sockPath)

	req := httptest.NewRequest(http.MethodGet, "/", http.NoBody)
	rec := httptest.NewRecorder()
	srv.mux.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", rec.Code)
	}
	body := rec.Body.String()
	if !strings.Contains(body, "dbgate Dashboard") {
		t.Error("response should contain dashboard title")
	}
	if !strings.Contains(body, "25.50") {
		t.Error("response should contain QPS value 25.50")
	}
}

func TestHandleIndex_NotFound(t *testing.T) {
	sockPath := startMockUDS(t, makeStatsResponse())
	srv := newTestServer(t, sockPath)

	req := httptest.NewRequest(http.MethodGet, "/nonexistent", http.NoBody)
	rec := httptest.NewRecorder()
	srv.mux.ServeHTTP(rec, req)

	if rec.Code != http.StatusNotFound {
		t.Errorf("expected 404, got %d", rec.Code)
	}
}

func TestHandleStats_OK(t *testing.T) {
	sockPath := startMockUDS(t, makeStatsResponse())
	srv := newTestServer(t, sockPath)

	req := httptest.NewRequest(http.MethodGet, "/api/stats", http.NoBody)
	rec := httptest.NewRecorder()
	srv.mux.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", rec.Code)
	}
	body := rec.Body.String()
	if !strings.Contains(body, "25.50") {
		t.Error("stats partial should contain QPS value")
	}
	if !strings.Contains(body, "1.20%") {
		t.Error("stats partial should contain block rate percentage")
	}
}

func TestHandleStats_Error(t *testing.T) {
	// Point to a non-existent socket to trigger connection error.
	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	c := client.NewClient("/nonexistent/path.sock", 500*time.Millisecond)
	srv := NewServer(":0", c, logger, "", "")

	req := httptest.NewRequest(http.MethodGet, "/api/stats", http.NoBody)
	rec := httptest.NewRecorder()
	srv.mux.ServeHTTP(rec, req)

	// Should still return 200 with error message (htmx expects 200).
	if rec.Code != http.StatusOK {
		t.Errorf("expected 200 even on error, got %d", rec.Code)
	}
	body := rec.Body.String()
	if !strings.Contains(body, "Failed to fetch") {
		t.Error("error partial should show error message")
	}
}

func TestHandleSessions_NotImplemented(t *testing.T) {
	respJSON, _ := json.Marshal(map[string]interface{}{
		"ok":    false,
		"error": "not implemented",
	})
	sockPath := startMockUDS(t, respJSON)
	srv := newTestServer(t, sockPath)

	req := httptest.NewRequest(http.MethodGet, "/api/sessions", http.NoBody)
	rec := httptest.NewRecorder()
	srv.mux.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", rec.Code)
	}
	body := rec.Body.String()
	if !strings.Contains(body, "Coming Soon") {
		t.Error("sessions should show 'Coming Soon' when not implemented")
	}
}

func TestHandleSessions_ConnectionError(t *testing.T) {
	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	c := client.NewClient("/nonexistent/path.sock", 500*time.Millisecond)
	srv := NewServer(":0", c, logger, "", "")

	req := httptest.NewRequest(http.MethodGet, "/api/sessions", http.NoBody)
	rec := httptest.NewRecorder()
	srv.mux.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", rec.Code)
	}
	body := rec.Body.String()
	if !strings.Contains(body, "Coming Soon") {
		t.Error("sessions should show 'Coming Soon' on connection error")
	}
}

func TestHandleChartData(t *testing.T) {
	sockPath := startMockUDS(t, makeStatsResponse())
	srv := newTestServer(t, sockPath)

	// First fetch stats to populate chart buffer.
	req := httptest.NewRequest(http.MethodGet, "/api/stats", http.NoBody)
	rec := httptest.NewRecorder()
	srv.mux.ServeHTTP(rec, req)

	// Now fetch chart data.
	req = httptest.NewRequest(http.MethodGet, "/api/chart-data", http.NoBody)
	rec = httptest.NewRecorder()
	srv.mux.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", rec.Code)
	}
	body := rec.Body.String()
	if !strings.Contains(body, "updateChart") {
		t.Error("chart data should contain updateChart call")
	}
}

func TestHandleStaticAssets(t *testing.T) {
	sockPath := startMockUDS(t, makeStatsResponse())
	srv := newTestServer(t, sockPath)

	tests := []struct {
		path        string
		contentType string
	}{
		{"/static/htmx.min.js", "text/javascript"},
		{"/static/pico.min.css", "text/css"},
		{"/static/dashboard.js", "text/javascript"},
	}

	for _, tt := range tests {
		t.Run(tt.path, func(t *testing.T) {
			req := httptest.NewRequest(http.MethodGet, tt.path, http.NoBody)
			rec := httptest.NewRecorder()
			srv.mux.ServeHTTP(rec, req)

			if rec.Code != http.StatusOK {
				t.Errorf("expected 200 for %s, got %d", tt.path, rec.Code)
			}
		})
	}
}

func TestChartBuffer(t *testing.T) {
	cb := newChartBuffer(3)

	// Add points.
	cb.add(1.0)
	cb.add(2.0)
	cb.add(3.0)

	snap := cb.snapshot()
	if len(snap) != 3 {
		t.Fatalf("expected 3 points, got %d", len(snap))
	}
	if snap[0].QPS != 1.0 || snap[1].QPS != 2.0 || snap[2].QPS != 3.0 {
		t.Errorf("unexpected values: %v", snap)
	}

	// Add one more — oldest should be dropped.
	cb.add(4.0)
	snap = cb.snapshot()
	if len(snap) != 3 {
		t.Fatalf("expected 3 points after overflow, got %d", len(snap))
	}
	if snap[0].QPS != 2.0 || snap[1].QPS != 3.0 || snap[2].QPS != 4.0 {
		t.Errorf("expected [2,3,4], got %v", snap)
	}
}

func TestChartBuffer_Snapshot_IsCopy(t *testing.T) {
	cb := newChartBuffer(5)
	cb.add(10.0)

	snap := cb.snapshot()
	snap[0].QPS = 999.0

	// Original should be unchanged.
	original := cb.snapshot()
	if original[0].QPS != 10.0 {
		t.Errorf("snapshot should be a copy, but original was modified: %v", original[0].QPS)
	}
}
