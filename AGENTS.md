# Agent instructions

## Idun terminal interaction

When working on the NE16/DORY validation flow on Idun, use the already-open
`idun-terminal` canvas. Do not open another terminal, start a nested SSH
session, or assume that Idun is unavailable because one output snapshot looks
stale.

`send_terminal_input` only confirms that input was delivered to the terminal
canvas. `read_terminal_output` returns a snapshot and may omit newly rendered
text or return an output region anchored to a previous read. An empty or
unchanged snapshot is not evidence that the command failed.

### Command verification protocol

For every important command, especially `sbatch`, `squeue`, `sacct`, builds,
and simulations:

1. Add a unique marker and preserve the command's exit status:

   ```bash
   id=CMD_123
   printf '__BEGIN_%s__\n' "$id"
   <command>
   rc=$?
   printf '__END_%s rc=%s__\n' "$id" "$rc"
   ```

2. Read the existing terminal with `mode: "since_last_input"`.
3. If the end marker is not present, read the same terminal again with
   `mode: "screen"` and then `mode: "full"`. Do not submit the command again.
4. If the marker is still absent, verify execution using durable evidence:
   - Slurm: `squeue`, `sacct`, and the configured output log.
   - Builds: executable timestamp, build log, or target output.
   - Simulations: the simulator log and the expected `PASS` marker.
5. Only report the command as unverified after all snapshot and durable-state
   checks fail. Ask the user to inspect the existing terminal rather than
   opening a replacement terminal.

Never duplicate a command merely because `read_terminal_output` did not show
its response. This is particularly important for Slurm submissions, where a
stale snapshot can otherwise create duplicate jobs.

### Idun-specific rules

- The shell is already logged into Idun; run commands at the existing prompt.
- Do not run `ssh mariusao@idun.hpc.ntnu.no` from that prompt.
- Prefer job IDs, `sacct` state/exit codes, and Slurm log files over terminal
  screenshots as the source of truth.
- Re-read a stable snapshot before concluding that a command or job is stuck.
