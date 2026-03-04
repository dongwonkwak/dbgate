// Package client provides a UDS client for communicating with the C++ dbgate core.
//
// Protocol: 4-byte LE length prefix + JSON body
//
//	Request:  [4byte LE len][JSON CommandRequest]
//	Response: [4byte LE len][JSON Response]
package client

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"time"
)

// Client is a Unix Domain Socket client for the dbgate control plane.
type Client struct {
	socketPath string
	timeout    time.Duration
}

// NewClient returns a new Client that connects to socketPath.
// timeout applies to the entire round-trip (dial + write + read).
func NewClient(socketPath string, timeout time.Duration) *Client {
	return &Client{
		socketPath: socketPath,
		timeout:    timeout,
	}
}

// SendCommand sends a simple command (no payload) to the C++ dbgate core and
// returns the parsed Response. The connection is closed after each call.
func (c *Client) SendCommand(cmd string) (*Response, error) {
	return c.sendRequest(CommandRequest{Command: cmd})
}

// sendRequest marshals req, writes it as a framed UDS message, reads the
// framed response, and returns the parsed Response.
// The connection is closed after each call.
func (c *Client) sendRequest(req CommandRequest) (*Response, error) {
	ctx, cancel := context.WithTimeout(context.Background(), c.timeout)
	defer cancel()

	conn, err := (&net.Dialer{}).DialContext(ctx, "unix", c.socketPath)
	if err != nil {
		return nil, fmt.Errorf("connect to %s: %w", c.socketPath, err)
	}
	defer func() {
		_ = conn.Close()
	}()

	// Apply deadline derived from context to the underlying connection.
	deadline, ok := ctx.Deadline()
	if ok {
		if err := conn.SetDeadline(deadline); err != nil {
			return nil, fmt.Errorf("set deadline: %w", err)
		}
	}

	// Marshal request.
	body, err := json.Marshal(req)
	if err != nil {
		return nil, fmt.Errorf("marshal request: %w", err)
	}

	// Write 4-byte LE length prefix.
	var lenBuf [4]byte
	if uint64(len(body)) > uint64(^uint32(0)) {
		return nil, fmt.Errorf("request body too large: %d", len(body))
	}
	reqLen := uint32(len(body)) // #nosec G115 -- bounded by the explicit check above.
	binary.LittleEndian.PutUint32(lenBuf[:], reqLen)
	if err := writeFull(conn, lenBuf[:]); err != nil {
		return nil, fmt.Errorf("write length prefix: %w", err)
	}

	// Write JSON body.
	if err := writeFull(conn, body); err != nil {
		return nil, fmt.Errorf("write request body: %w", err)
	}

	// Read 4-byte LE length prefix of response.
	if _, err := io.ReadFull(conn, lenBuf[:]); err != nil {
		return nil, fmt.Errorf("read response length: %w", err)
	}
	respLen := binary.LittleEndian.Uint32(lenBuf[:])

	const maxResponseBytes = 16 * 1024 * 1024 // 16 MiB guard
	if respLen == 0 || respLen > maxResponseBytes {
		return nil, fmt.Errorf("invalid response length %d", respLen)
	}

	// Read JSON body.
	respBody := make([]byte, respLen)
	if _, err := io.ReadFull(conn, respBody); err != nil {
		return nil, fmt.Errorf("read response body: %w", err)
	}

	var resp Response
	if err := json.Unmarshal(respBody, &resp); err != nil {
		return nil, fmt.Errorf("parse response JSON: %w", err)
	}

	return &resp, nil
}

// PolicyExplain sends a "policy_explain" command with the given SQL, user, and
// sourceIP and returns the decoded PolicyExplainResult.
// This is a dry-run evaluation — no actual blocking occurs.
func (c *Client) PolicyExplain(sql, user, sourceIP string) (*PolicyExplainResult, error) {
	req := CommandRequest{
		Command: "policy_explain",
		Payload: PolicyExplainRequest{
			SQL:      sql,
			User:     user,
			SourceIP: sourceIP,
		},
	}
	resp, err := c.sendRequest(req)
	if err != nil {
		return nil, err
	}
	if !resp.OK {
		errMsg := resp.Error
		if errMsg == "" {
			errMsg = "unknown server error"
		}
		return nil, fmt.Errorf("policy_explain: server error: %s", errMsg)
	}
	if resp.Payload == nil {
		return nil, fmt.Errorf("policy_explain: response has no payload")
	}

	// Re-marshal the payload interface{} so we can unmarshal into PolicyExplainResult.
	payloadBytes, err := json.Marshal(resp.Payload)
	if err != nil {
		return nil, fmt.Errorf("policy_explain: re-marshal payload: %w", err)
	}

	var result PolicyExplainResult
	if err := json.Unmarshal(payloadBytes, &result); err != nil {
		return nil, fmt.Errorf("policy_explain: parse payload: %w", err)
	}

	return &result, nil
}

// writeFull writes all bytes in buf to w, looping until all bytes are written
// or an error occurs. This handles the rare case where Write returns n < len(buf)
// without an error, which technically violates the io.Writer contract but can
// occur on some platforms or under resource pressure.
func writeFull(w io.Writer, buf []byte) error {
	for len(buf) > 0 {
		n, err := w.Write(buf)
		buf = buf[n:]
		if err != nil {
			return err
		}
	}
	return nil
}

// rawStats is an intermediate struct that handles the C++ serialization quirk:
// captured_at is sent as captured_at_ms (Unix epoch milliseconds), not as an
// RFC 3339 string. All other fields are identical to StatsSnapshot.
type rawStats struct {
	TotalConnections uint64  `json:"total_connections"`
	ActiveSessions   uint64  `json:"active_sessions"`
	TotalQueries     uint64  `json:"total_queries"`
	BlockedQueries   uint64  `json:"blocked_queries"`
	QPS              float64 `json:"qps"`
	BlockRate        float64 `json:"block_rate"`
	// C++ side serialises the timestamp as Unix epoch milliseconds.
	CapturedAtMs int64 `json:"captured_at_ms"`
}

// GetStats sends a "stats" command and returns the decoded StatsSnapshot.
func (c *Client) GetStats() (*StatsSnapshot, error) {
	resp, err := c.SendCommand("stats")
	if err != nil {
		return nil, err
	}
	if !resp.OK {
		return nil, fmt.Errorf("server error: %s", resp.Error)
	}
	if resp.Payload == nil {
		return nil, fmt.Errorf("stats response has no payload")
	}

	// Re-marshal the payload interface{} so we can unmarshal into rawStats.
	payloadBytes, err := json.Marshal(resp.Payload)
	if err != nil {
		return nil, fmt.Errorf("re-marshal stats payload: %w", err)
	}

	var raw rawStats
	if err := json.Unmarshal(payloadBytes, &raw); err != nil {
		return nil, fmt.Errorf("parse stats payload: %w", err)
	}

	snap := &StatsSnapshot{
		TotalConnections: raw.TotalConnections,
		ActiveSessions:   raw.ActiveSessions,
		TotalQueries:     raw.TotalQueries,
		BlockedQueries:   raw.BlockedQueries,
		QPS:              raw.QPS,
		BlockRate:        raw.BlockRate,
		CapturedAt:       time.UnixMilli(raw.CapturedAtMs).UTC(),
	}
	return snap, nil
}
