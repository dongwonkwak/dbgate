// Package client defines types for UDS communication with the C++ dbgate core.
//
// Protocol: 4-byte LE length prefix + JSON body
//
// Request:  CommandRequest  -> JSON -> [4byte LE len][JSON]
// Response: Response        <- JSON <- [4byte LE len][JSON]
//
// Supported commands: "stats" | "sessions" | "policy_reload"
package client

import (
	"time"
)

// StatsSnapshot is the JSON representation of C++ StatsCollector.snapshot().
// Fields must match the C++ UDS response payload exactly.
type StatsSnapshot struct {
	TotalConnections uint64    `json:"total_connections"`
	ActiveSessions   uint64    `json:"active_sessions"`
	TotalQueries     uint64    `json:"total_queries"`
	BlockedQueries   uint64    `json:"blocked_queries"`
	QPS              float64   `json:"qps"`
	BlockRate        float64   `json:"block_rate"`
	CapturedAt       time.Time `json:"captured_at"`
}

// CommandRequest is a UDS request sent to the C++ dbgate core.
// Version is optional; defaults to 1 if omitted.
type CommandRequest struct {
	Command string `json:"command"`           // "stats" | "sessions" | "policy_reload"
	Version int    `json:"version,omitempty"` // protocol version, default 1
}

// Response is the common UDS response wrapper from the C++ dbgate core.
// On success: OK=true,  Payload contains the result.
// On failure: OK=false, Error contains a diagnostic message.
type Response struct {
	OK      bool        `json:"ok"`
	Error   string      `json:"error,omitempty"`
	Payload interface{} `json:"payload,omitempty"`
}
