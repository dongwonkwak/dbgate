package client

import (
	"encoding/binary"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// startMockServer starts a mock UDS server that accepts a single connection,
// drains the request, and responds with respPayload (raw bytes, already framed).
// It returns the socket path. The server goroutine exits after serving one request.
func startMockServer(t *testing.T, respPayload []byte) string {
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

	go func() {
		conn, err := ln.Accept()
		if err != nil {
			return
		}
		defer func() { _ = conn.Close() }()

		// Drain the request: 4-byte LE length prefix + body.
		var lenBuf [4]byte
		if _, err := readFull(conn, lenBuf[:]); err != nil {
			return
		}
		reqLen := binary.LittleEndian.Uint32(lenBuf[:])
		reqBody := make([]byte, reqLen)
		if _, err := readFull(conn, reqBody); err != nil {
			return
		}

		// Write the pre-built response frame.
		_, _ = conn.Write(respPayload)
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

// frameResponse wraps jsonBody with a 4-byte LE length prefix.
func frameResponse(jsonBody []byte) []byte {
	frame := make([]byte, 4+len(jsonBody))
	binary.LittleEndian.PutUint32(frame[:4], uint32(len(jsonBody)))
	copy(frame[4:], jsonBody)
	return frame
}

// TestSendCommand_OKResponse verifies that SendCommand correctly reads a framed
// OK response from the mock server.
func TestSendCommand_OKResponse(t *testing.T) {
	respJSON := []byte(`{"ok":true,"payload":{"key":"value"}}`)
	sockPath := startMockServer(t, frameResponse(respJSON))

	c := NewClient(sockPath, 3*time.Second)
	resp, err := c.SendCommand("stats")
	if err != nil {
		t.Fatalf("SendCommand: %v", err)
	}
	if !resp.OK {
		t.Errorf("expected OK=true, got false")
	}
}

// TestFraming verifies that the client sends the correct 4-byte LE framing and
// includes the expected command field in the request JSON.
func TestFraming(t *testing.T) {
	respJSON := []byte(`{"ok":true}`)

	dir := t.TempDir()
	sockPath := filepath.Join(dir, "framing.sock")
	ln, err := net.Listen("unix", sockPath)
	if err != nil {
		t.Fatalf("listen: %v", err)
	}

	type receivedMsg struct {
		body []byte
		err  error
	}
	received := make(chan receivedMsg, 1)

	go func() {
		defer func() { _ = ln.Close() }()
		conn, err := ln.Accept()
		if err != nil {
			received <- receivedMsg{err: err}
			return
		}
		defer func() { _ = conn.Close() }()

		var lenBuf [4]byte
		if _, err := readFull(conn, lenBuf[:]); err != nil {
			received <- receivedMsg{err: err}
			return
		}
		bodyLen := binary.LittleEndian.Uint32(lenBuf[:])
		body := make([]byte, bodyLen)
		if _, err := readFull(conn, body); err != nil {
			received <- receivedMsg{err: err}
			return
		}
		received <- receivedMsg{body: body}

		// Write back a minimal OK response.
		_, _ = conn.Write(frameResponse(respJSON))
	}()

	c := NewClient(sockPath, 3*time.Second)
	_, err = c.SendCommand("stats")
	if err != nil {
		t.Fatalf("SendCommand: %v", err)
	}

	select {
	case msg := <-received:
		if msg.err != nil {
			t.Fatalf("server read error: %v", msg.err)
		}
		var req CommandRequest
		if err := json.Unmarshal(msg.body, &req); err != nil {
			t.Fatalf("parse request body: %v", err)
		}
		if req.Command != "stats" {
			t.Errorf("expected command 'stats', got %q", req.Command)
		}
	default:
		t.Fatal("server did not receive request")
	}
}

// TestGetStats_CapturedAtMs verifies that captured_at_ms (epoch ms) is
// correctly converted to time.Time.
func TestGetStats_CapturedAtMs(t *testing.T) {
	nowMs := int64(1740830400000) // 2025-03-01 12:00:00 UTC in ms
	payload := map[string]interface{}{
		"total_connections": 10,
		"active_sessions":   2,
		"total_queries":     1000,
		"blocked_queries":   50,
		"qps":               12.5,
		"block_rate":        5.0,
		"captured_at_ms":    nowMs,
	}
	wrapper := map[string]interface{}{
		"ok":      true,
		"payload": payload,
	}
	respJSON, err := json.Marshal(wrapper)
	if err != nil {
		t.Fatalf("marshal mock response: %v", err)
	}

	sockPath := startMockServer(t, frameResponse(respJSON))

	c := NewClient(sockPath, 3*time.Second)
	snap, err := c.GetStats()
	if err != nil {
		t.Fatalf("GetStats: %v", err)
	}

	expectedTime := time.UnixMilli(nowMs).UTC()
	if !snap.CapturedAt.Equal(expectedTime) {
		t.Errorf("CapturedAt: got %v, want %v", snap.CapturedAt, expectedTime)
	}
	if snap.TotalQueries != 1000 {
		t.Errorf("TotalQueries: got %d, want 1000", snap.TotalQueries)
	}
	if snap.BlockedQueries != 50 {
		t.Errorf("BlockedQueries: got %d, want 50", snap.BlockedQueries)
	}
	if snap.ActiveSessions != 2 {
		t.Errorf("ActiveSessions: got %d, want 2", snap.ActiveSessions)
	}
}

// TestGetStats_ServerError verifies that a non-OK server response surfaces as an error.
func TestGetStats_ServerError(t *testing.T) {
	respJSON := []byte(`{"ok":false,"error":"internal error"}`)
	sockPath := startMockServer(t, frameResponse(respJSON))

	c := NewClient(sockPath, 3*time.Second)
	_, err := c.GetStats()
	if err == nil {
		t.Fatal("expected error, got nil")
	}
}

// TestTimeout verifies that the client returns an error when the server is slow.
func TestTimeout(t *testing.T) {
	dir := t.TempDir()
	sockPath := filepath.Join(dir, "timeout.sock")
	ln, err := net.Listen("unix", sockPath)
	if err != nil {
		t.Fatalf("listen: %v", err)
	}

	t.Cleanup(func() { _ = ln.Close() })

	// Accept connection but never respond — simulates a hung server.
	go func() {
		conn, err := ln.Accept()
		if err != nil {
			return
		}
		// Hold the connection open without writing anything.
		time.Sleep(10 * time.Second)
		_ = conn.Close()
	}()

	c := NewClient(sockPath, 100*time.Millisecond)
	start := time.Now()
	_, err = c.SendCommand("stats")
	elapsed := time.Since(start)

	if err == nil {
		t.Fatal("expected timeout error, got nil")
	}
	// Should not have waited more than 1 second.
	if elapsed > 1*time.Second {
		t.Errorf("timeout took too long: %v", elapsed)
	}
}
