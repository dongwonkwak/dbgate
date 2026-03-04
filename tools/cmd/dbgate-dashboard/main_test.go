package main

import (
	"encoding/binary"
	"encoding/json"
	"flag"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// mockUDSServer starts a mock Unix Domain Socket server that accepts connections,
// drains each request frame, and responds with respJSON.
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
				if _, err := drainFull(c, hdr[:]); err != nil {
					return
				}
				reqBody := make([]byte, binary.LittleEndian.Uint32(hdr[:]))
				if _, err := drainFull(c, reqBody); err != nil {
					return
				}
				_, _ = c.Write(frame)
			}(conn)
		}
	}()

	return sockPath
}

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

func TestMockUDSServer_RespondsCorrectly(t *testing.T) {
	resp := map[string]interface{}{
		"ok": true,
		"payload": map[string]interface{}{
			"qps": 10.0,
		},
	}
	respJSON, _ := json.Marshal(resp)
	sockPath := mockUDSServer(t, respJSON)

	conn, err := net.DialTimeout("unix", sockPath, 3*time.Second)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer func() { _ = conn.Close() }()

	// Send framed request.
	reqJSON := []byte(`{"command":"stats"}`)
	var lenBuf [4]byte
	binary.LittleEndian.PutUint32(lenBuf[:], uint32(len(reqJSON)))
	if _, err := conn.Write(lenBuf[:]); err != nil {
		t.Fatalf("write len: %v", err)
	}
	if _, err := conn.Write(reqJSON); err != nil {
		t.Fatalf("write body: %v", err)
	}

	// Read framed response.
	if _, err := drainFull(conn, lenBuf[:]); err != nil {
		t.Fatalf("read resp len: %v", err)
	}
	respLen := binary.LittleEndian.Uint32(lenBuf[:])
	respBody := make([]byte, respLen)
	if _, err := drainFull(conn, respBody); err != nil {
		t.Fatalf("read resp body: %v", err)
	}

	var result map[string]interface{}
	if err := json.Unmarshal(respBody, &result); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if result["ok"] != true {
		t.Errorf("expected ok=true, got %v", result["ok"])
	}
}

func TestRun_MissingAuth_Fails(t *testing.T) {
	// run() must fail-closed: refuse to start when auth credentials are absent.
	origArgs := os.Args
	origFlags := flag.CommandLine
	t.Cleanup(func() {
		os.Args = origArgs
		flag.CommandLine = origFlags
		os.Unsetenv("DASHBOARD_AUTH_USER")
		os.Unsetenv("DASHBOARD_AUTH_PASSWORD")
	})

	os.Unsetenv("DASHBOARD_AUTH_USER")
	os.Unsetenv("DASHBOARD_AUTH_PASSWORD")
	flag.CommandLine = flag.NewFlagSet("test", flag.ContinueOnError)
	os.Args = []string{"dbgate-dashboard"} // no --auth-user / --auth-password

	err := run()
	if err == nil {
		t.Fatal("expected error when auth credentials missing, got nil")
	}
	if !strings.Contains(err.Error(), "auth is required") {
		t.Errorf("unexpected error message: %v", err)
	}
}
