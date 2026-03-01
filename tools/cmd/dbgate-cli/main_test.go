package main

import (
	"encoding/binary"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// mockUDSServer starts a mock Unix Domain Socket server that accepts one
// connection, drains the request frame, and responds with respJSON (framed).
func mockUDSServer(t *testing.T, respJSON []byte) string {
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

	// Pre-build the framed response: 4-byte LE length + JSON body.
	frame := make([]byte, 4+len(respJSON))
	binary.LittleEndian.PutUint32(frame[:4], uint32(len(respJSON)))
	copy(frame[4:], respJSON)

	go func() {
		conn, err := ln.Accept()
		if err != nil {
			return
		}
		defer func() { _ = conn.Close() }()

		// Drain the request: 4-byte LE prefix + body.
		var hdr [4]byte
		if _, err := drainFull(conn, hdr[:]); err != nil {
			return
		}
		reqBody := make([]byte, binary.LittleEndian.Uint32(hdr[:]))
		if _, err := drainFull(conn, reqBody); err != nil {
			return
		}

		_, _ = conn.Write(frame)
	}()

	return sockPath
}

// drainFull reads exactly len(buf) bytes from conn.
func drainFull(conn net.Conn, buf []byte) (int, error) {
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

// TestRunGenericCommand_OK verifies that a successful server response (ok=true)
// causes runGenericCommand to return nil.
func TestRunGenericCommand_OK(t *testing.T) {
	respJSON, _ := json.Marshal(map[string]interface{}{"ok": true})
	sockPath := mockUDSServer(t, respJSON)

	if err := runGenericCommand(sockPath, 3*time.Second, "sessions"); err != nil {
		t.Fatalf("expected nil error, got: %v", err)
	}
}

// TestRunGenericCommand_ServerError verifies that ok=false with an error message
// is returned as a non-nil error (non-zero exit code for automation).
func TestRunGenericCommand_ServerError(t *testing.T) {
	respJSON, _ := json.Marshal(map[string]interface{}{
		"ok":    false,
		"error": "not implemented",
	})
	sockPath := mockUDSServer(t, respJSON)

	err := runGenericCommand(sockPath, 3*time.Second, "sessions")
	if err == nil {
		t.Fatal("expected error for ok=false, got nil")
	}
	if !strings.Contains(err.Error(), "server error") {
		t.Errorf("error should contain 'server error', got: %v", err)
	}
}

// TestRunGenericCommand_ServerError_EmptyMsg verifies that ok=false with no
// error field still returns a non-nil error containing "not implemented".
func TestRunGenericCommand_ServerError_EmptyMsg(t *testing.T) {
	respJSON, _ := json.Marshal(map[string]interface{}{"ok": false})
	sockPath := mockUDSServer(t, respJSON)

	err := runGenericCommand(sockPath, 3*time.Second, "policy_reload")
	if err == nil {
		t.Fatal("expected error for ok=false with empty error field, got nil")
	}
	if !strings.Contains(err.Error(), "not implemented") {
		t.Errorf("error should contain 'not implemented', got: %v", err)
	}
}

// TestRunGenericCommand_ConnectionError verifies that an unreachable socket
// path returns a non-nil error.
func TestRunGenericCommand_ConnectionError(t *testing.T) {
	err := runGenericCommand("/nonexistent/path.sock", 500*time.Millisecond, "sessions")
	if err == nil {
		t.Fatal("expected error for unreachable socket, got nil")
	}
}
