package dashboard

import (
	"encoding/json"
	"log/slog"
	"net/http"
	"sync"
	"time"

	"github.com/dongwonkwak/dbgate/tools/internal/client"
)

// indexData is the top-level data passed to the index template.
type indexData struct {
	Stats *client.StatsSnapshot
	Error string
}

// sessionsData is passed to the sessions partial.
type sessionsData struct {
	Sessions []sessionRow
	Error    string
}

// sessionRow represents one active session for table display.
type sessionRow struct {
	ID       string
	Client   string
	User     string
	Database string
	Duration string
	Queries  int
}

// chartPoint is a single data point in the QPS history.
type chartPoint struct {
	QPS float64 `json:"qps"`
	T   int64   `json:"t"` // Unix epoch seconds
}

// chartBuffer is a thread-safe ring buffer for QPS history.
type chartBuffer struct {
	mu     sync.Mutex
	points []chartPoint
	cap    int
}

func newChartBuffer(capacity int) *chartBuffer {
	return &chartBuffer{
		points: make([]chartPoint, 0, capacity),
		cap:    capacity,
	}
}

// add appends a new QPS sample. If the buffer is full, the oldest point is dropped.
func (cb *chartBuffer) add(qps float64) {
	cb.mu.Lock()
	defer cb.mu.Unlock()

	pt := chartPoint{QPS: qps, T: time.Now().Unix()}
	if len(cb.points) >= cb.cap {
		copy(cb.points, cb.points[1:])
		cb.points[len(cb.points)-1] = pt
	} else {
		cb.points = append(cb.points, pt)
	}
}

// snapshot returns a copy of the current chart data.
func (cb *chartBuffer) snapshot() []chartPoint {
	cb.mu.Lock()
	defer cb.mu.Unlock()

	out := make([]chartPoint, len(cb.points))
	copy(out, cb.points)
	return out
}

// handleIndex renders the full dashboard page.
func (s *Server) handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}

	tmpl, err := parseTemplates()
	if err != nil {
		s.logger.Error("parse templates", slog.String("error", err.Error()))
		http.Error(w, "internal server error", http.StatusInternalServerError)
		return
	}

	data := indexData{}
	stats, err := s.client.GetStats()
	if err != nil {
		data.Error = err.Error()
		s.logger.Warn("get stats for index", slog.String("error", err.Error()))
	} else {
		data.Stats = stats
		s.chart.add(stats.QPS)
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := tmpl.ExecuteTemplate(w, "templates/layout.html", data); err != nil {
		s.logger.Error("execute template", slog.String("error", err.Error()))
	}
}

// handleStats renders the stats partial for htmx polling.
func (s *Server) handleStats(w http.ResponseWriter, _ *http.Request) {
	tmpl, err := parseTemplates()
	if err != nil {
		s.logger.Error("parse templates", slog.String("error", err.Error()))
		http.Error(w, "internal server error", http.StatusInternalServerError)
		return
	}

	data := indexData{}
	stats, err := s.client.GetStats()
	if err != nil {
		data.Error = err.Error()
		s.logger.Warn("get stats", slog.String("error", err.Error()))
	} else {
		data.Stats = stats
		s.chart.add(stats.QPS)
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := tmpl.ExecuteTemplate(w, "templates/partials/stats.html", data); err != nil {
		s.logger.Error("execute stats template", slog.String("error", err.Error()))
	}
}

// handleSessions renders the sessions partial. Returns "Coming Soon" if the
// C++ side returns a 501-equivalent (ok=false).
func (s *Server) handleSessions(w http.ResponseWriter, _ *http.Request) {
	tmpl, err := parseTemplates()
	if err != nil {
		s.logger.Error("parse templates", slog.String("error", err.Error()))
		http.Error(w, "internal server error", http.StatusInternalServerError)
		return
	}

	data := sessionsData{}
	resp, err := s.client.SendCommand("sessions")
	if err != nil {
		data.Error = err.Error()
	} else if !resp.OK {
		data.Error = "not implemented"
	}
	// TODO: parse resp.Payload into sessionRow slice when C++ side implements sessions

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := tmpl.ExecuteTemplate(w, "templates/partials/sessions.html", data); err != nil {
		s.logger.Error("execute sessions template", slog.String("error", err.Error()))
	}
}

// handleChartData returns the QPS history as a JSON array embedded in a script tag
// that calls updateChart() on the client side.
func (s *Server) handleChartData(w http.ResponseWriter, _ *http.Request) {
	tmpl, err := parseTemplates()
	if err != nil {
		s.logger.Error("parse templates", slog.String("error", err.Error()))
		http.Error(w, "internal server error", http.StatusInternalServerError)
		return
	}

	points := s.chart.snapshot()
	// Marshal as JSON for the template to inject into the script.
	jsonData, err := json.Marshal(points)
	if err != nil {
		s.logger.Error("marshal chart data", slog.String("error", err.Error()))
		http.Error(w, "internal server error", http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := tmpl.ExecuteTemplate(w, "templates/partials/chart.html", json.RawMessage(jsonData)); err != nil {
		s.logger.Error("execute chart template", slog.String("error", err.Error()))
	}
}
