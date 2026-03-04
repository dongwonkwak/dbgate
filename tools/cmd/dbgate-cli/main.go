// Command dbgate-cli is a CLI management tool for the dbgate proxy.
//
// It communicates with the C++ dbgate core via Unix Domain Socket using a
// 4-byte LE length-prefixed JSON protocol.
//
// Usage:
//
//	dbgate-cli [--socket /tmp/dbgate.sock] [--timeout 5s] <command>
//
// Commands:
//
//	stats                  Print QPS, block rate, active sessions, and query counters.
//	sessions               List active sessions (server-side not yet implemented).
//	policy reload          Trigger a policy reload (server-side not yet implemented).
//	policy explain         Dry-run SQL evaluation against the policy engine.
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"time"

	"github.com/dongwonkwak/dbgate/tools/internal/client"
	"github.com/spf13/cobra"
)

const (
	defaultSocket  = "/tmp/dbgate.sock"
	defaultTimeout = 5 * time.Second
)

func main() {
	if err := newRootCmd().Execute(); err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}
}

func newRootCmd() *cobra.Command {
	var socketPath string
	var timeout time.Duration

	root := &cobra.Command{
		Use:   "dbgate-cli",
		Short: "CLI management tool for the dbgate proxy",
		Long: `dbgate-cli connects to the dbgate proxy via Unix Domain Socket and
provides commands to inspect statistics, list sessions, and reload policies.`,
		SilenceUsage:  true,
		SilenceErrors: true,
	}

	root.PersistentFlags().StringVar(&socketPath, "socket", defaultSocket, "Path to dbgate Unix Domain Socket")
	root.PersistentFlags().DurationVar(&timeout, "timeout", defaultTimeout, "Timeout for UDS requests")

	// stats subcommand
	statsCmd := &cobra.Command{
		Use:   "stats",
		Short: "Print proxy statistics (QPS, block rate, active sessions, etc.)",
		RunE: func(cmd *cobra.Command, args []string) error {
			return runStats(socketPath, timeout)
		},
	}

	// sessions subcommand
	sessionsCmd := &cobra.Command{
		Use:   "sessions",
		Short: "List active sessions",
		RunE: func(cmd *cobra.Command, args []string) error {
			return runGenericCommand(socketPath, timeout, "sessions")
		},
	}

	// policy subcommand (parent)
	policyCmd := &cobra.Command{
		Use:   "policy",
		Short: "Policy management commands",
	}

	// policy reload subcommand
	policyReloadCmd := &cobra.Command{
		Use:   "reload",
		Short: "Reload the access control policy",
		RunE: func(cmd *cobra.Command, args []string) error {
			return runGenericCommand(socketPath, timeout, "policy_reload")
		},
	}

	// policy explain subcommand
	var explainSQL string
	var explainUser string
	var explainIP string
	var explainJSON bool

	policyExplainCmd := &cobra.Command{
		Use:   "explain",
		Short: "Dry-run SQL evaluation against the policy engine",
		Long: `Evaluate a SQL statement against the current policy without executing it.
Useful for debugging policy rules and auditing access control decisions.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return runPolicyExplain(socketPath, timeout, explainSQL, explainUser, explainIP, explainJSON)
		},
	}
	policyExplainCmd.Flags().StringVar(&explainSQL, "sql", "", "SQL statement to evaluate (required)")
	policyExplainCmd.Flags().StringVar(&explainUser, "user", "", "MySQL username (required)")
	policyExplainCmd.Flags().StringVar(&explainIP, "ip", "", "Client IPv4 address (required)")
	policyExplainCmd.Flags().BoolVar(&explainJSON, "json", false, "Output raw JSON response")
	if err := policyExplainCmd.MarkFlagRequired("sql"); err != nil {
		panic(err)
	}
	if err := policyExplainCmd.MarkFlagRequired("user"); err != nil {
		panic(err)
	}
	if err := policyExplainCmd.MarkFlagRequired("ip"); err != nil {
		panic(err)
	}

	policyCmd.AddCommand(policyReloadCmd, policyExplainCmd)
	root.AddCommand(statsCmd, sessionsCmd, policyCmd)

	return root
}

// runStats executes the "stats" command and prints the result in human-readable format.
func runStats(socketPath string, timeout time.Duration) error {
	c := client.NewClient(socketPath, timeout)
	snap, err := c.GetStats()
	if err != nil {
		return fmt.Errorf("stats: %w", err)
	}

	fmt.Println("=== dbgate stats ===")
	fmt.Printf("QPS:              %8.2f\n", snap.QPS)
	fmt.Printf("Block Rate:       %7.2f%%\n", snap.BlockRate*100)
	fmt.Printf("Active Sessions:  %8d\n", snap.ActiveSessions)
	fmt.Printf("Total Queries:    %8d\n", snap.TotalQueries)
	fmt.Printf("Blocked Queries:  %8d\n", snap.BlockedQueries)
	fmt.Printf("Monitored Blocks: %8d\n", snap.MonitoredBlocks)
	fmt.Printf("Total Connections:%8d\n", snap.TotalConnections)
	fmt.Printf("Captured At:      %s\n", snap.CapturedAt.Format("2006-01-02 15:04:05 UTC"))

	return nil
}

// runPolicyExplain evaluates a SQL statement against the policy engine (dry-run)
// and prints the result in human-readable or JSON format.
func runPolicyExplain(socketPath string, timeout time.Duration, sql, user, ip string, asJSON bool) error {
	c := client.NewClient(socketPath, timeout)
	result, err := c.PolicyExplain(sql, user, ip)
	if err != nil {
		return fmt.Errorf("policy explain: %w", err)
	}

	if asJSON {
		enc := json.NewEncoder(os.Stdout)
		enc.SetIndent("", "  ")
		if err := enc.Encode(result); err != nil {
			return fmt.Errorf("encode JSON: %w", err)
		}
		return nil
	}

	action := result.Action
	if result.MonitorMode {
		action += " [MONITOR MODE]"
	}
	fmt.Printf("Action  : %s\n", action)
	fmt.Printf("Rule    : %s\n", result.MatchedRule)
	fmt.Printf("Reason  : %s\n", result.Reason)
	if result.MatchedAccessRule != "" {
		fmt.Printf("Access  : %s\n", result.MatchedAccessRule)
	}
	fmt.Printf("Path    : %s\n", result.EvaluationPath)
	if result.ParsedCommand != "" {
		fmt.Printf("Command : %s\n", result.ParsedCommand)
	}
	if len(result.ParsedTables) > 0 {
		fmt.Printf("Tables  : %v\n", result.ParsedTables)
	}
	return nil
}

// runGenericCommand sends a raw command to the server and prints the response.
// Any non-OK response from the server is returned as an error so that callers
// (including shell scripts and CI pipelines) receive a non-zero exit code.
func runGenericCommand(socketPath string, timeout time.Duration, cmd string) error {
	c := client.NewClient(socketPath, timeout)
	resp, err := c.SendCommand(cmd)
	if err != nil {
		return fmt.Errorf("%s: %w", cmd, err)
	}

	if !resp.OK {
		errMsg := resp.Error
		if errMsg == "" {
			errMsg = "not implemented"
		}
		return fmt.Errorf("%s: server error: %s", cmd, errMsg)
	}

	fmt.Printf("[%s] OK\n", cmd)
	if resp.Payload != nil {
		fmt.Printf("payload: %v\n", resp.Payload)
	}
	return nil
}
