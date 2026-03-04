// Package client defines types for UDS communication with the C++ dbgate core.
//
// Protocol: 4-byte LE length prefix + JSON body
//
// Request:  CommandRequest  -> JSON -> [4byte LE len][JSON]
// Response: Response        <- JSON <- [4byte LE len][JSON]
//
// Supported commands: "stats" | "policy_explain" | "sessions" | "policy_reload"
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
// Payload is used by commands such as policy_explain that require input parameters.
type CommandRequest struct {
	Command string      `json:"command"`           // "stats" | "policy_explain" | "sessions" | "policy_reload"
	Version int         `json:"version,omitempty"` // protocol version, default 1
	Payload interface{} `json:"payload,omitempty"` // optional command payload
}

// PolicyExplainRequest is the request payload for the "policy_explain" command.
// All three fields are required by the C++ policy engine.
type PolicyExplainRequest struct {
	SQL      string `json:"sql"`       // SQL statement to evaluate
	User     string `json:"user"`      // MySQL username
	SourceIP string `json:"source_ip"` // Client IPv4 address
}

// PolicyExplainResult is the response payload for the "policy_explain" command.
// It describes the policy engine's evaluation of the given SQL statement.
type PolicyExplainResult struct {
	Action            string   `json:"action"`                   // "allow" | "block" | "log"
	MatchedRule       string   `json:"matched_rule"`             // rule ID used for the decision
	Reason            string   `json:"reason"`                   // human-readable decision reason
	MatchedAccessRule string   `json:"matched_access_rule"`      // "user@cidr" or empty string
	EvaluationPath    string   `json:"evaluation_path"`          // step-by-step evaluation trace
	ParsedCommand     string   `json:"parsed_command,omitempty"` // e.g. "SELECT", "DROP"
	ParsedTables      []string `json:"parsed_tables,omitempty"`  // extracted table names
}

// Response is the common UDS response wrapper from the C++ dbgate core.
// On success: OK=true,  Payload contains the result.
// On failure: OK=false, Error contains a diagnostic message.
type Response struct {
	OK      bool        `json:"ok"`
	Error   string      `json:"error,omitempty"`
	Payload interface{} `json:"payload,omitempty"`
}
