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
//	stats                        Print QPS, block rate, active sessions, and query counters.
//	sessions                     List active sessions (server-side not yet implemented).
//	policy reload                Trigger a policy reload and print the new version.
//	policy explain               Dry-run SQL evaluation against the policy engine.
//	policy versions              List all stored policy versions.
//	policy rollback --version N  Roll back to a specific policy version.
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"text/tabwriter"
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
			return runPolicyReload(socketPath, timeout)
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

	// policy versions subcommand
	policyVersionsCmd := &cobra.Command{
		Use:   "versions",
		Short: "List all stored policy versions",
		RunE: func(cmd *cobra.Command, args []string) error {
			return runPolicyVersions(socketPath, timeout)
		},
	}

	// policy rollback subcommand
	var rollbackVersion uint64
	policyRollbackCmd := &cobra.Command{
		Use:   "rollback",
		Short: "Roll back to a specific policy version",
		RunE: func(cmd *cobra.Command, args []string) error {
			return runPolicyRollback(socketPath, timeout, rollbackVersion)
		},
	}
	policyRollbackCmd.Flags().Uint64Var(&rollbackVersion, "version", 0, "Target policy version to roll back to (required)")
	if err := policyRollbackCmd.MarkFlagRequired("version"); err != nil {
		panic(err)
	}

	policyCmd.AddCommand(policyReloadCmd, policyExplainCmd, policyVersionsCmd, policyRollbackCmd)
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

// runPolicyReload triggers a policy reload and prints version information.
func runPolicyReload(socketPath string, timeout time.Duration) error {
	c := client.NewClient(socketPath, timeout)
	result, err := c.PolicyReload()
	if err != nil {
		return fmt.Errorf("policy reload: %w", err)
	}

	fmt.Printf("Policy reloaded successfully (version %d)\n", result.Version)
	fmt.Printf("Rules count: %d\n", result.RulesCount)
	if result.Message != "" {
		fmt.Printf("Message: %s\n", result.Message)
	}
	return nil
}

// runPolicyVersions lists all stored policy versions.
func runPolicyVersions(socketPath string, timeout time.Duration) error {
	c := client.NewClient(socketPath, timeout)
	result, err := c.PolicyVersions()
	if err != nil {
		return fmt.Errorf("policy versions: %w", err)
	}

	fmt.Printf("Current version: %d\n", result.Current)
	if len(result.Versions) == 0 {
		fmt.Println("No version history available.")
		return nil
	}

	fmt.Println("=== Policy Versions ===")
	w := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
	fmt.Fprintln(w, "Version\tTimestamp\tRules\tHash")
	for _, v := range result.Versions {
		hash := v.Hash
		if len(hash) > 8 {
			hash = hash[:8] + "..."
		}
		fmt.Fprintf(w, "%d\t%s\t%d\t%s\n", v.Version, v.Timestamp, v.RulesCount, hash)
	}
	if err := w.Flush(); err != nil {
		return fmt.Errorf("flush output: %w", err)
	}
	return nil
}

// runPolicyRollback rolls back the policy to a specific version.
func runPolicyRollback(socketPath string, timeout time.Duration, targetVersion uint64) error {
	c := client.NewClient(socketPath, timeout)
	result, err := c.PolicyRollback(targetVersion)
	if err != nil {
		return fmt.Errorf("policy rollback: %w", err)
	}

	fmt.Printf("Rolled back to version %d (from version %d)\n",
		result.RolledBackTo, result.PreviousVersion)
	fmt.Printf("Rules count: %d\n", result.RulesCount)
	return nil
}
