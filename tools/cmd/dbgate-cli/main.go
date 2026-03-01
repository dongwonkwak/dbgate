// Command dbgate-cli is a CLI management tool for the dbgate proxy.
//
// It communicates with the C++ dbgate core via Unix Domain Socket using a
// 4-byte LE length-prefixed JSON protocol.
//
// Usage:
//
//	dbgate-cli [--socket /var/run/dbgate/dbgate.sock] [--timeout 5s] <command>
//
// Commands:
//
//	stats          Print QPS, block rate, active sessions, and query counters.
//	sessions       List active sessions (server-side not yet implemented).
//	policy reload  Trigger a policy reload (server-side not yet implemented).
package main

import (
	"fmt"
	"os"
	"time"

	"github.com/dongwonkwak/dbgate/tools/internal/client"
	"github.com/spf13/cobra"
)

const (
	defaultSocket  = "/var/run/dbgate/dbgate.sock"
	defaultTimeout = 5 * time.Second
)

func main() {
	if err := newRootCmd().Execute(); err != nil {
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

	policyCmd.AddCommand(policyReloadCmd)
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
	fmt.Printf("Block Rate:       %7.2f%%\n", snap.BlockRate)
	fmt.Printf("Active Sessions:  %8d\n", snap.ActiveSessions)
	fmt.Printf("Total Queries:    %8d\n", snap.TotalQueries)
	fmt.Printf("Blocked Queries:  %8d\n", snap.BlockedQueries)
	fmt.Printf("Total Connections:%8d\n", snap.TotalConnections)
	fmt.Printf("Captured At:      %s\n", snap.CapturedAt.Format("2006-01-02 15:04:05 UTC"))

	return nil
}

// runGenericCommand sends a raw command to the server and prints the response.
// For commands that are not yet implemented server-side (501), it prints a
// friendly message instead of a raw error.
func runGenericCommand(socketPath string, timeout time.Duration, cmd string) error {
	c := client.NewClient(socketPath, timeout)
	resp, err := c.SendCommand(cmd)
	if err != nil {
		return fmt.Errorf("%s: %w", cmd, err)
	}

	if !resp.OK {
		// Detect the common "not implemented" placeholder from the C++ side.
		printNotImplemented(cmd, resp.Error)
		return nil
	}

	fmt.Printf("[%s] OK\n", cmd)
	if resp.Payload != nil {
		fmt.Printf("payload: %v\n", resp.Payload)
	}
	return nil
}

// printNotImplemented prints a user-friendly message for server-side 501 responses.
func printNotImplemented(cmd, serverMsg string) {
	if serverMsg == "" {
		serverMsg = "not implemented"
	}
	fmt.Printf("[%s] %s (code 501)\n", cmd, serverMsg)
}
