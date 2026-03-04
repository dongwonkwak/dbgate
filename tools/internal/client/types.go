// Package client defines types for UDS communication with the C++ dbgate core.
//
// Protocol: 4-byte LE length prefix + JSON body
//
// Request:  CommandRequest  -> JSON -> [4byte LE len][JSON]
// Response: Response        <- JSON <- [4byte LE len][JSON]
//
// Supported commands: "stats" | "policy_explain" | "sessions" | "policy_reload" |
// "policy_versions" | "policy_rollback"
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
	MonitoredBlocks  uint64    `json:"monitored_blocks"`
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
	MonitorMode       bool     `json:"monitor_mode"`             // true when policy engine is in monitor-only mode
}

// Response is the common UDS response wrapper from the C++ dbgate core.
// On success: OK=true,  Payload contains the result.
// On failure: OK=false, Error contains a diagnostic message.
type Response struct {
	OK      bool        `json:"ok"`
	Error   string      `json:"error,omitempty"`
	Payload interface{} `json:"payload,omitempty"`
}

// PolicyVersionMeta represents metadata for a stored policy version.
type PolicyVersionMeta struct {
	Version    uint64 `json:"version"`
	Timestamp  string `json:"timestamp"`
	RulesCount uint32 `json:"rules_count"`
	Hash       string `json:"hash"`
}

// PolicyVersionsResult is the response payload for "policy_versions".
type PolicyVersionsResult struct {
	Current  uint64              `json:"current"`
	Versions []PolicyVersionMeta `json:"versions"`
}

// PolicyRollbackResult is the response payload for "policy_rollback".
type PolicyRollbackResult struct {
	RolledBackTo    uint64 `json:"rolled_back_to"`
	PreviousVersion uint64 `json:"previous_version"`
	RulesCount      uint32 `json:"rules_count"`
}

// PolicyRollbackRequest is the request payload for "policy_rollback".
type PolicyRollbackRequest struct {
	TargetVersion uint64 `json:"target_version"`
}

// PolicyReloadResult is the response payload for "policy_reload".
type PolicyReloadResult struct {
	ReloadedAt string `json:"reloaded_at"`
	RulesCount uint32 `json:"rules_count"`
	Version    uint64 `json:"version"`
	Message    string `json:"message"`
}
