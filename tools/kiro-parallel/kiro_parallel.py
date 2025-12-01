#!/usr/bin/env python3
"""
kiro-parallel: Parallel Task Orchestrator for cc-sdd

Reads tasks.md from cc-sdd specs, analyzes dependencies,
and executes tasks in parallel waves using tmux + git worktrees.

Usage:
    kiro-parallel analyze <spec-name> [--mermaid]
    kiro-parallel run <spec-name> [--max-parallel=4] [--dry-run] [--wave=N]
    kiro-parallel status <spec-name>
    kiro-parallel clean <spec-name>
"""

import os
import re
import json
import subprocess
import time
import argparse
import threading
from pathlib import Path
from dataclasses import dataclass, field
from typing import Optional
from collections import defaultdict
from enum import Enum
from datetime import datetime


class TaskStatus(Enum):
    PENDING = "pending"
    RUNNING = "running"
    COMPLETED = "completed"
    FAILED = "failed"
    STOPPED = "stopped"
    BLOCKED = "blocked"


@dataclass
class Task:
    id: str
    title: str
    description: str = ""
    dependencies: list[str] = field(default_factory=list)
    status: TaskStatus = TaskStatus.PENDING
    wave: int = -1
    session_name: str = ""
    requirements: list[str] = field(default_factory=list)
    is_parallel: bool = False  # Marked with (P)
    parent_id: str = ""  # For subtasks like 2.1 -> parent is 2
    is_parent_task: bool = False  # True if this is a grouping header (has subtasks)
    start_time: float = 0.0


@dataclass
class ExecutionPlan:
    spec_name: str
    tasks: dict[str, Task]
    waves: list[list[str]]
    total_waves: int = 0
    

class TasksParser:
    """Parse cc-sdd tasks.md format - handles hierarchical tasks"""
    
    def __init__(self, kiro_dir: str = ".kiro"):
        self.kiro_dir = Path(kiro_dir)
    
    def find_tasks_file(self, spec_name: str) -> Path:
        """Find tasks.md for a given spec"""
        tasks_path = self.kiro_dir / "specs" / spec_name / "tasks.md"
        if not tasks_path.exists():
            raise FileNotFoundError(f"Tasks file not found: {tasks_path}")
        return tasks_path
    
    def parse(self, spec_name: str) -> dict[str, Task]:
        """Parse tasks.md and return dict of Task objects"""
        tasks_path = self.find_tasks_file(spec_name)
        content = tasks_path.read_text()
        
        tasks = {}
        current_task = None
        current_description_lines = []
        
        lines = content.split('\n')
        
        for line in lines:
            # Match task lines: - [x] 1. Title or - [ ] 2.1 (P) Title
            task_match = re.match(
                r'^-\s*\[([ xX])\]\s*(\d+(?:\.\d+)?)\s*(?:\(P\))?\s*(.+)$',
                line
            )
            
            if task_match:
                # Save previous task's description
                if current_task and current_description_lines:
                    current_task.description = '\n'.join(current_description_lines).strip()
                
                completed = task_match.group(1).lower() == 'x'
                task_id = task_match.group(2)
                title = task_match.group(3).strip()
                is_parallel = '(P)' in line
                
                # Determine parent ID for subtasks
                parent_id = ""
                if '.' in task_id:
                    parent_id = task_id.split('.')[0]
                
                status = TaskStatus.COMPLETED if completed else TaskStatus.PENDING
                
                current_task = Task(
                    id=task_id,
                    title=title,
                    status=status,
                    is_parallel=is_parallel,
                    parent_id=parent_id
                )
                tasks[task_id] = current_task
                current_description_lines = []
                continue
            
            # Match dependency lines: _Depends on: 2.3 (description)_
            dep_match = re.match(r'^\s*[-_]\s*Depends on:\s*(.+?)_?\s*$', line, re.IGNORECASE)
            if dep_match and current_task:
                deps_text = dep_match.group(1)
                # Extract task IDs like "2.3" from text
                dep_ids = re.findall(r'(\d+(?:\.\d+)?)', deps_text)
                current_task.dependencies.extend(dep_ids)
                continue
            
            # Match requirements: _Requirements: 1.1, 1.2, 1.3_
            req_match = re.match(r'^\s*[-_]\s*Requirements?:\s*(.+?)_?\s*$', line, re.IGNORECASE)
            if req_match and current_task:
                reqs_text = req_match.group(1)
                reqs = re.findall(r'[\d.]+', reqs_text)
                current_task.requirements.extend(reqs)
                continue
            
            # Collect description lines (indented content under task)
            if current_task and line.strip().startswith('-') and not line.strip().startswith('- ['):
                current_description_lines.append(line.strip()[1:].strip())
        
        # Save last task's description
        if current_task and current_description_lines:
            current_task.description = '\n'.join(current_description_lines).strip()

        # Identify parent tasks (grouping headers with subtasks)
        # A task is a parent if it has no dot in its ID and has children with that ID prefix
        for task in tasks.values():
            if '.' not in task.id:
                # Check if any task has this as parent_id
                has_children = any(t.parent_id == task.id for t in tasks.values())
                if has_children:
                    task.is_parent_task = True

        # Add implicit dependencies: subtasks depend on parent task
        for task in tasks.values():
            if task.parent_id and task.parent_id in tasks:
                # Subtask implicitly depends on parent being defined
                # But also check if parent has other subtasks that should complete first
                pass

            # Clean up dependencies
            task.dependencies = list(set(task.dependencies))

        # Add implicit dependencies for sequential subtasks within same parent
        self._add_implicit_dependencies(tasks)

        return tasks
    
    def _add_implicit_dependencies(self, tasks: dict[str, Task]):
        """Add implicit sequential dependencies for subtasks"""
        # Group subtasks by parent
        subtasks_by_parent = defaultdict(list)
        for task in tasks.values():
            if task.parent_id:
                subtasks_by_parent[task.parent_id].append(task)
        
        # For each parent, subtasks without explicit deps depend on previous subtask
        # unless marked as parallel (P)
        for parent_id, subtasks in subtasks_by_parent.items():
            # Sort by task ID
            subtasks.sort(key=lambda t: float(t.id))
            
            for i, task in enumerate(subtasks):
                if i > 0 and not task.dependencies and not task.is_parallel:
                    # No explicit dependency and not parallel - depends on previous
                    prev_task = subtasks[i - 1]
                    if prev_task.id not in task.dependencies:
                        task.dependencies.append(prev_task.id)


class DependencyAnalyzer:
    """Analyze task dependencies and create execution waves"""
    
    def analyze(self, tasks: dict[str, Task]) -> ExecutionPlan:
        """Build execution plan with parallel waves"""
        # Filter to only incomplete tasks
        pending_tasks = {
            tid: task for tid, task in tasks.items()
            if task.status != TaskStatus.COMPLETED
        }
        
        if not pending_tasks:
            print("All tasks are completed!")
            return ExecutionPlan(spec_name="", tasks=tasks, waves=[], total_waves=0)
        
        # Validate dependencies exist
        self._validate_dependencies(pending_tasks, tasks)
        
        # Check for cycles
        if self._has_cycle(pending_tasks):
            raise ValueError("Circular dependency detected in tasks!")
        
        # Calculate waves using topological sort
        waves = self._calculate_waves(pending_tasks, tasks)
        
        # Update task wave assignments
        for wave_idx, task_ids in enumerate(waves):
            for task_id in task_ids:
                pending_tasks[task_id].wave = wave_idx
        
        return ExecutionPlan(
            spec_name="",
            tasks=tasks,
            waves=waves,
            total_waves=len(waves)
        )
    
    def _validate_dependencies(self, pending_tasks: dict[str, Task], all_tasks: dict[str, Task]):
        """Ensure all dependencies reference existing tasks or are completed"""
        for task in pending_tasks.values():
            valid_deps = []
            for dep in task.dependencies:
                if dep in all_tasks:
                    # Only keep dependency if the dep task is not completed
                    if all_tasks[dep].status != TaskStatus.COMPLETED:
                        valid_deps.append(dep)
                    # If dep is completed, we don't need to wait for it
                else:
                    print(f"Warning: Task {task.id} depends on non-existent Task {dep}")
            task.dependencies = valid_deps
    
    def _has_cycle(self, tasks: dict[str, Task]) -> bool:
        """Detect circular dependencies using DFS"""
        WHITE, GRAY, BLACK = 0, 1, 2
        color = {task_id: WHITE for task_id in tasks}
        
        def dfs(task_id: str) -> bool:
            color[task_id] = GRAY
            for dep in tasks[task_id].dependencies:
                if dep not in color:
                    continue
                if color[dep] == GRAY:
                    return True
                if color[dep] == WHITE and dfs(dep):
                    return True
            color[task_id] = BLACK
            return False
        
        for task_id in tasks:
            if color[task_id] == WHITE:
                if dfs(task_id):
                    return True
        return False
    
    def _calculate_waves(self, pending_tasks: dict[str, Task], all_tasks: dict[str, Task]) -> list[list[str]]:
        """Group tasks into parallel execution waves"""
        wave_map = {}
        
        def get_wave(task_id: str, visited: set) -> int:
            if task_id in wave_map:
                return wave_map[task_id]
            
            if task_id in visited:
                return 0
            
            visited.add(task_id)
            task = pending_tasks.get(task_id)
            
            if not task:
                return -1  # Task doesn't exist or is completed
            
            if not task.dependencies:
                wave_map[task_id] = 0
                return 0
            
            max_dep_wave = -1
            for dep in task.dependencies:
                if dep in pending_tasks:
                    dep_wave = get_wave(dep, visited)
                    max_dep_wave = max(max_dep_wave, dep_wave)
            
            wave_map[task_id] = max_dep_wave + 1
            return wave_map[task_id]
        
        for task_id in pending_tasks:
            get_wave(task_id, set())
        
        # Group by wave
        waves_dict = defaultdict(list)
        for task_id, wave in wave_map.items():
            if wave >= 0:
                waves_dict[wave].append(task_id)
        
        # Convert to sorted list
        if not waves_dict:
            return []
        
        max_wave = max(waves_dict.keys())
        waves = []
        for i in range(max_wave + 1):
            wave_tasks = sorted(waves_dict[i], key=lambda x: float(x))
            waves.append(wave_tasks)
        
        return waves


class ParallelExecutor:
    """Execute tasks in parallel using tmux and git worktrees"""
    
    def __init__(
        self,
        spec_name: str,
        project_root: str = ".",
        max_parallel: int = 2,
        use_par: bool = True
    ):
        self.spec_name = spec_name
        self.project_root = Path(project_root).resolve()
        self.max_parallel = max_parallel
        self.use_par = use_par and self._check_par_installed()
        self.state_file = self.project_root / ".kiro" / "specs" / spec_name / ".parallel_state.json"
    
    def _check_par_installed(self) -> bool:
        """Check if par CLI is available"""
        try:
            subprocess.run(["par", "--version"], capture_output=True, check=True)
            return True
        except (subprocess.CalledProcessError, FileNotFoundError):
            return False

    def _count_running(self, tasks: dict[str, Task]) -> int:
        """Count tasks currently marked as running"""
        return sum(1 for task in tasks.values() if task.status == TaskStatus.RUNNING)
    
    def _list_active_sessions(self) -> set[str]:
        """List active tmux/par sessions"""
        sessions: set[str] = set()
        
        if self.use_par:
            result = self._run_cmd(["par", "ls"], check=False)
            if result.returncode != 0 or not result.stdout:
                return sessions
            for line in result.stdout.splitlines():
                parts = line.strip().split()
                if parts:
                    sessions.add(parts[0])
            return sessions
        
        # tmux
        result = self._run_cmd(["tmux", "ls"], check=False)
        if result.returncode != 0 or not result.stdout:
            return sessions
        for line in result.stdout.splitlines():
            name = line.split(":", 1)[0].strip()
            if name:
                sessions.add(name)
        return sessions
    
    def refresh_session_status(self, tasks: dict[str, Task]):
        """Refresh task statuses based on live tmux/par sessions"""
        active_sessions = self._list_active_sessions()
        for task in tasks.values():
            if task.session_name:
                if task.session_name in active_sessions:
                    task.status = TaskStatus.RUNNING
                elif task.status == TaskStatus.RUNNING:
                    task.status = TaskStatus.STOPPED

    def get_logs_dir(self) -> Path:
        """Get the logs directory for this spec"""
        logs_dir = self.project_root / ".kiro" / "specs" / self.spec_name / "logs"
        logs_dir.mkdir(parents=True, exist_ok=True)
        return logs_dir

    def extract_token_summary(self, output: str) -> dict:
        """Extract token usage statistics from session output"""
        summary = {
            "max_tokens": 0,
            "token_readings": [],
            "duration_seconds": 0,
            "thinking_time": 0,
        }

        # Find all token readings (e.g., "64708 tokens", "96295 tokens")
        token_pattern = r'(\d+)\s*tokens'
        matches = re.findall(token_pattern, output)
        if matches:
            summary["token_readings"] = [int(t) for t in matches]
            summary["max_tokens"] = max(summary["token_readings"])

        # Find thinking time mentions (e.g., "Thinking… (esc to interrupt · 4m 19s")
        thinking_pattern = r'Thinking.*?(\d+)m\s*(\d+)s'
        thinking_matches = re.findall(thinking_pattern, output)
        if thinking_matches:
            # Get the last (longest) thinking time
            for mins, secs in thinking_matches:
                total_secs = int(mins) * 60 + int(secs)
                summary["thinking_time"] = max(summary["thinking_time"], total_secs)

        # Find duration from elapsed time patterns
        elapsed_pattern = r'(\d+):(\d+):(\d+)'
        elapsed_matches = re.findall(elapsed_pattern, output)
        if elapsed_matches:
            # Get the last elapsed time
            h, m, s = elapsed_matches[-1]
            summary["duration_seconds"] = int(h) * 3600 + int(m) * 60 + int(s)

        return summary

    def format_token_summary(self, summary: dict, task: Task) -> str:
        """Format token summary as a readable string"""
        lines = [
            "",
            "=" * 60,
            f"SESSION SUMMARY - Task {task.id}",
            "=" * 60,
            f"Task: {task.title}",
            f"Session: {task.session_name}",
            "",
        ]

        if summary["max_tokens"] > 0:
            lines.append(f"Total Tokens Used: {summary['max_tokens']:,}")
            if len(summary["token_readings"]) > 1:
                lines.append(f"Token Readings: {len(summary['token_readings'])} samples")
                lines.append(f"  First: {summary['token_readings'][0]:,}")
                lines.append(f"  Last:  {summary['token_readings'][-1]:,}")

        if summary["thinking_time"] > 0:
            mins, secs = divmod(summary["thinking_time"], 60)
            lines.append(f"Max Thinking Time: {mins}m {secs}s")

        if summary["duration_seconds"] > 0:
            hours, remainder = divmod(summary["duration_seconds"], 3600)
            mins, secs = divmod(remainder, 60)
            lines.append(f"Session Duration: {hours:02d}:{mins:02d}:{secs:02d}")

        # Calculate cost estimate (rough estimate based on Claude pricing)
        if summary["max_tokens"] > 0:
            # Rough estimate: $15/1M input tokens, $75/1M output tokens for Opus
            # Assuming 70% input, 30% output
            input_tokens = int(summary["max_tokens"] * 0.7)
            output_tokens = int(summary["max_tokens"] * 0.3)
            cost_estimate = (input_tokens * 15 + output_tokens * 75) / 1_000_000
            lines.append(f"Estimated Cost: ~${cost_estimate:.2f}")

        lines.append("=" * 60)
        return "\n".join(lines)

    def capture_session_output(self, task: Task, lines: int = 10000, silent: bool = False, create_timestamped: bool = True) -> str:
        """Capture tmux session output and save to file with token summary.

        Args:
            task: The task to capture output from
            lines: Number of lines to capture from scrollback
            silent: If True, don't print summary to console
            create_timestamped: If True, also create timestamped log file (for final capture)
        """
        if not task.session_name:
            return ""

        result = self._run_cmd([
            "tmux", "capture-pane", "-t", task.session_name,
            "-p", "-S", f"-{lines}"
        ], check=False)

        if result.returncode != 0:
            return ""

        output = result.stdout

        # Extract and format token summary
        token_summary = self.extract_token_summary(output)
        summary_text = self.format_token_summary(token_summary, task)

        # Append summary to output
        output_with_summary = output + summary_text

        # Save to log file
        logs_dir = self.get_logs_dir()

        # Always update the "latest" file (for continuous monitoring)
        latest_file = logs_dir / f"task-{task.id.replace('.', '-')}_latest.log"
        latest_file.write_text(output_with_summary)

        # Create timestamped file only when requested (final capture)
        if create_timestamped:
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            log_file = logs_dir / f"task-{task.id.replace('.', '-')}_{timestamp}.log"
            log_file.write_text(output_with_summary)

        # Print summary to console unless silent
        if not silent:
            print(summary_text)

        return output

    def update_running_logs(self, tasks: dict[str, Task], wave_task_ids: list[str]):
        """Update log files for all running tasks (called periodically during watch)."""
        for tid in wave_task_ids:
            task = tasks.get(tid)
            if task and task.status == TaskStatus.RUNNING and task.session_name:
                # Silent capture, only update latest file (no timestamped file)
                self.capture_session_output(task, silent=True, create_timestamped=False)

    def update_all_running_logs(self, tasks: dict[str, Task]):
        """Update log files for ALL running tasks (for background refresh)."""
        for task in tasks.values():
            if task.status == TaskStatus.RUNNING and task.session_name:
                self.capture_session_output(task, silent=True, create_timestamped=False)

    def is_session_waiting_for_input(self, task: Task) -> bool:
        """Check if session is waiting for user input/confirmation.

        Detects when Claude Code is at an idle prompt waiting for user to press Enter
        to start execution, or waiting for y/n confirmation, or waiting for
        slash command tool permission approval.
        """
        if not task.session_name:
            return False

        result = self._run_cmd([
            "tmux", "capture-pane", "-t", task.session_name,
            "-p", "-S", "-30"
        ], check=False)

        if result.returncode != 0:
            return False

        output = result.stdout
        lines = output.strip().split('\n')
        last_lines = '\n'.join(lines[-10:]) if len(lines) >= 10 else output

        # Check if actively working (these states mean NOT waiting)
        active_patterns = [
            r'Thinking',           # Actively thinking
            r'Coalescing',         # Processing results
            r'Searching',          # Searching
            r'Reading',            # Reading files
            r'Writing',            # Writing files
            r'⏺',                  # Tool execution indicator (filled circle)
        ]

        for pattern in active_patterns:
            if re.search(pattern, last_lines, re.IGNORECASE):
                return False  # Actively working, not waiting

        # Check for "0 tokens" which means command entered but not started
        if re.search(r'\b0\s*tokens\b', last_lines):
            return True  # At idle prompt, needs Enter to start

        # Check for slash command "running..." which waits for tool permission
        # Pattern: "> /command is running…" without "Allowed" following it
        if re.search(r'is running[…\.]+', last_lines):
            # If we see "running" but no "Allowed" after it, it's waiting for approval
            if not re.search(r'Allowed \d+ tools', last_lines):
                return True

        # Check for confirmation prompts
        confirmation_patterns = [
            r'\(y/n\)',            # Yes/no prompt
            r'\[Y/n\]',            # Yes/no prompt
            r'\[y/N\]',            # Yes/no prompt
            r'Press Enter',        # Press enter prompt
            r'Continue\?',         # Continue prompt
        ]

        for pattern in confirmation_patterns:
            if re.search(pattern, last_lines, re.IGNORECASE):
                return True

        return False

    def send_enter_if_waiting(self, task: Task) -> bool:
        """Send Enter key if session is waiting for input"""
        if self.is_session_waiting_for_input(task):
            self._run_cmd([
                "tmux", "send-keys", "-t", task.session_name, "Enter"
            ], check=False)
            return True
        return False

    def is_task_completed(self, task: Task) -> bool:
        """Check if task has completed its work.

        A task is considered complete when:
        1. The session no longer exists, OR
        2. The session shows completion indicators AND is at an idle prompt
        """
        if not task.session_name:
            return False

        # Check if session still exists
        active_sessions = self._list_active_sessions()
        if task.session_name not in active_sessions:
            return True  # Session ended = task stopped (may be complete or failed)

        # Check output for completion indicators
        result = self._run_cmd([
            "tmux", "capture-pane", "-t", task.session_name,
            "-p", "-S", "-50"
        ], check=False)

        if result.returncode != 0:
            return False

        output = result.stdout
        lines = output.strip().split('\n')
        # Use last 50 lines to catch completion markers that may have scrolled up
        last_lines = '\n'.join(lines[-50:]) if len(lines) >= 50 else output

        # Check for completion indicators FIRST (before active check)
        # This prevents false negatives when completion message contains active-like patterns
        completion_patterns = [
            r'⎿\s*Done',              # Claude Code completion marker
            r'Done \(\d+ tool uses',  # Done with stats
            r'Task.*completed',
            r'Implementation complete',
            r'Implementation Complete',
            r'All tests pass',
            r'✅.*complete',
            r'Remaining tasks',       # Shows next steps = current done
            r'Next Steps',            # Shows next steps = current done
            r'successfully implemented',
            r'completed successfully',
        ]

        # Check if "Done" marker is present - this is definitive completion
        for pattern in completion_patterns:
            if re.search(pattern, last_lines, re.IGNORECASE):
                # Verify we're at prompt (not in middle of output)
                for line in reversed(lines[-5:]):
                    stripped = line.strip()
                    if stripped.startswith('>') or stripped == '>':
                        return True
                # High token count with Done marker = completed
                token_match = re.search(r'(\d+)\s*tokens', last_lines)
                if token_match and int(token_match.group(1)) > 5000:
                    return True

        # Check if actively working (NOT completed)
        # Claude Code has various thinking indicators with different verbs
        active_patterns = [
            r'Thinking[…\.]',         # Thinking... or Thinking…
            r'Transfiguring[…\.]',    # Another thinking state
            r'Ebbing[…\.]',           # Another thinking state
            r'Coalescing[…\.]',       # Processing state
            r'Pondering[…\.]',        # Another thinking state
            r'Weaving[…\.]',          # Another thinking state
            r'Conjuring[…\.]',        # Another thinking state
            r'\w+ing[…\.]\s*\(esc',   # Any "Verbing… (esc to interrupt" pattern
            r'Searching',
            r'Reading',
            r'Writing',
            r'⏺\s+\w',                # Tool execution: ⏺ followed by action word
        ]

        for pattern in active_patterns:
            if re.search(pattern, last_lines):
                return False  # Still working

        # Check if at idle prompt with high token count (no active patterns = likely done)
        # Method 1: Check for prompt character at end
        for line in reversed(lines[-5:]):
            stripped = line.strip()
            if stripped.startswith('>') or stripped == '>':
                # At prompt - if we have significant tokens, task is done
                token_match = re.search(r'(\d+)\s*tokens', last_lines)
                if token_match and int(token_match.group(1)) > 10000:
                    return True

        # Method 2: Check for SESSION SUMMARY - this definitively means task ended
        if re.search(r'SESSION SUMMARY', last_lines):
            return True

        return False

    def commit_and_merge_task(self, task: Task) -> bool:
        """Commit changes and merge task branch to main"""
        safe_id = task.id.replace('.', '-')
        branch_name = f"task-{self.spec_name}-{safe_id}"
        worktree_path = self.project_root.parent / f"worktrees/{self.spec_name}/task-{safe_id}"

        if not worktree_path.exists():
            print(f"  ⚠️  Worktree not found for task {task.id}")
            return False

        # Check for changes in worktree
        result = subprocess.run(
            ["git", "status", "--porcelain"],
            capture_output=True, text=True,
            cwd=str(worktree_path)
        )

        if result.stdout.strip():
            # There are uncommitted changes - commit them
            print(f"  📝 Committing changes for task {task.id}...")

            # Add all changes
            subprocess.run(
                ["git", "add", "-A"],
                cwd=str(worktree_path),
                check=False
            )

            # Commit
            commit_msg = f"""feat: Implement Task {task.id} - {task.title}

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude <noreply@anthropic.com>
"""
            subprocess.run(
                ["git", "commit", "-m", commit_msg],
                cwd=str(worktree_path),
                check=False
            )

        # Merge to main
        print(f"  🔀 Merging task {task.id} branch to main...")

        # Switch to main in the main project
        self._run_cmd(["git", "checkout", "main"], check=False)

        # Merge the task branch
        result = self._run_cmd(
            ["git", "merge", branch_name, "--no-edit"],
            check=False
        )

        if result.returncode == 0:
            print(f"  ✅ Task {task.id} merged successfully")
            task.status = TaskStatus.COMPLETED
            return True
        else:
            print(f"  ⚠️  Merge conflict or error for task {task.id}")
            print(f"      {result.stderr.strip()}")
            return False

    def kill_session(self, task: Task):
        """Kill the tmux session for a task"""
        if not task.session_name:
            return

        # Capture final output before killing
        self.capture_session_output(task)

        if self.use_par:
            self._run_cmd(["par", "rm", task.session_name], check=False)
        else:
            self._run_cmd(["tmux", "kill-session", "-t", task.session_name], check=False)

        print(f"  🗑️  Killed session for task {task.id}")

    def process_completed_task(self, task: Task) -> bool:
        """Full workflow: capture output, commit, merge, kill session"""
        print(f"\n📋 Processing completed task {task.id}...")

        # Mark task as completed FIRST so state is correct
        task.status = TaskStatus.COMPLETED

        # 1. Capture session output
        self.capture_session_output(task)
        print(f"  📄 Output saved to logs/task-{task.id.replace('.', '-')}_latest.log")

        # 2. Commit and merge
        merged = self.commit_and_merge_task(task)

        # 3. Kill session
        self.kill_session(task)

        return merged

    def _run_cmd(self, cmd: list[str], check: bool = True) -> subprocess.CompletedProcess:
        """Run a shell command"""
        return subprocess.run(cmd, capture_output=True, text=True, check=check, cwd=str(self.project_root))
    
    def create_worktree(self, task: Task) -> str:
        """Create git worktree for a task"""
        safe_id = task.id.replace('.', '-')
        branch_name = f"task-{self.spec_name}-{safe_id}"
        worktree_path = self.project_root.parent / f"worktrees/{self.spec_name}/task-{safe_id}"
        
        worktree_path.parent.mkdir(parents=True, exist_ok=True)
        
        if worktree_path.exists():
            self._run_cmd(["git", "worktree", "remove", str(worktree_path), "--force"], check=False)
        
        # Delete branch if exists
        self._run_cmd(["git", "branch", "-D", branch_name], check=False)
        
        # Create new worktree
        result = self._run_cmd([
            "git", "worktree", "add",
            "-b", branch_name,
            str(worktree_path)
        ], check=False)
        
        if result.returncode != 0:
            print(f"    Warning: {result.stderr.strip()}")
        
        return str(worktree_path)
    
    def create_session(self, task: Task, worktree_path: str) -> str:
        """Create tmux session for a task"""
        safe_id = task.id.replace('.', '-')
        session_name = f"{self.spec_name}-task-{safe_id}"
        task.session_name = session_name
        
        # Kill existing session if present
        self._run_cmd(["tmux", "kill-session", "-t", session_name], check=False)
        
        if self.use_par:
            self._run_cmd(["par", "new", session_name, "-d", worktree_path], check=False)
        else:
            self._run_cmd([
                "tmux", "new-session",
                "-d", "-s", session_name,
                "-c", worktree_path
            ], check=False)
        
        return session_name
    
    def send_command(self, session_name: str, command: str):
        """Send command to a tmux session"""
        if self.use_par:
            self._run_cmd(["par", "send", session_name, command], check=False)
        else:
            self._run_cmd(["tmux", "send-keys", "-t", session_name, command, "Enter"], check=False)

    def _capture_pane(self, session_name: str) -> str:
        """Capture current pane content from a tmux session"""
        try:
            result = subprocess.run(
                ["tmux", "capture-pane", "-t", session_name, "-p"],
                capture_output=True, text=True, timeout=5
            )
            return result.stdout if result.returncode == 0 else ""
        except Exception:
            return ""

    def start_task(self, task: Task):
        """Start execution of a single task"""
        worktree_path = self.create_worktree(task)
        session_name = self.create_session(task, worktree_path)

        time.sleep(1)

        # Start Claude Code
        self.send_command(session_name, "/usr/local/bin/c")

        # Wait for Claude Code to be ready (check for prompt)
        max_wait = 10  # seconds
        for i in range(max_wait):
            time.sleep(1)
            output = self._capture_pane(session_name)
            # Check if Claude Code prompt is visible (shows "> " at the end)
            if output and re.search(r'^>\s*$', output, re.MULTILINE):
                break

        # Send the implementation command
        impl_cmd = f"/kiro:spec-impl {self.spec_name} {task.id}"
        self.send_command(session_name, impl_cmd)
        
        task.start_time = time.time()
        task.status = TaskStatus.RUNNING
        print(f"  ✓ Started Task {task.id}: {task.title[:50]}...")
        print(f"    Session: {session_name}")
        print(f"    Worktree: {worktree_path}")

        # Create initial log file immediately
        time.sleep(1)  # Brief wait for session to initialize
        self.capture_session_output(task, silent=True, create_timestamped=False)
        print(f"    Log: {self.get_logs_dir()}/task-{task.id.replace('.', '-')}_latest.log")
    
    def _start_pending_tasks(self, tasks: dict[str, Task], wave_task_ids: list[str], announce: bool = True) -> int:
        """Start pending tasks respecting max_parallel and current running count.

        Note: Parent tasks (grouping headers with subtasks) are auto-completed since
        they have no implementation work - only their subtasks have actual work.
        """
        # Auto-complete parent tasks (they're just grouping headers)
        for tid in wave_task_ids:
            task = tasks[tid]
            if task.status == TaskStatus.PENDING and task.is_parent_task:
                task.status = TaskStatus.COMPLETED
                if announce:
                    print(f"  ⏭️  Skipping parent task {task.id} (grouping header - subtasks will be executed)")

        pending_tasks = [
            tasks[tid] for tid in wave_task_ids
            if tasks[tid].status == TaskStatus.PENDING and not tasks[tid].is_parent_task
        ]

        if not pending_tasks:
            if announce:
                print("  All tasks in this wave already completed or running")
            return 0

        running_count = self._count_running(tasks)
        available_slots = max(self.max_parallel - running_count, 0)

        if available_slots <= 0:
            if announce:
                print(f"  No available slots (currently running: {running_count}, max parallel: {self.max_parallel})")
                print("  Wait for running tasks to finish, then rerun this command to start more.")
            return 0

        batch = pending_tasks[:available_slots]
        if announce:
            print(f"\n  Starting batch of {len(batch)} tasks (slots available: {available_slots}):\n")

        for task in batch:
            self.start_task(task)
            if announce:
                print()

        remaining = len(pending_tasks) - len(batch)
        if announce and remaining > 0:
            print(f"  {remaining} task(s) in this wave are pending. Rerun this command after some tasks finish to start more.")

        return len(batch)
    
    def save_state(self, plan: ExecutionPlan):
        """Save execution state to file"""
        state = {
            "spec_name": plan.spec_name,
            "waves": plan.waves,
            "tasks": {
                task_id: {
                    "id": task.id,
                    "title": task.title,
                    "status": task.status.value,
                    "wave": task.wave,
                    "session_name": task.session_name,
                    "start_time": task.start_time
                }
                for task_id, task in plan.tasks.items()
            }
        }
        self.state_file.parent.mkdir(parents=True, exist_ok=True)
        self.state_file.write_text(json.dumps(state, indent=2))
    
    def load_state(self) -> Optional[dict]:
        """Load execution state from file"""
        if self.state_file.exists():
            return json.loads(self.state_file.read_text())
        return None
    
    def execute_wave(self, wave_idx: int, tasks: dict[str, Task], wave_task_ids: list[str]):
        """Execute all tasks in a wave"""
        print(f"\n{'='*60}")
        print(f"🌊 Wave {wave_idx + 1}: {len(wave_task_ids)} tasks")
        print(f"{'='*60}")
        
        self._start_pending_tasks(tasks, wave_task_ids, announce=True)
    
    def cleanup(self, tasks: dict[str, Task]):
        """Clean up all sessions and worktrees"""
        print("\nCleaning up...")
        
        for task in tasks.values():
            if task.session_name:
                if self.use_par:
                    self._run_cmd(["par", "rm", task.session_name], check=False)
                else:
                    self._run_cmd(["tmux", "kill-session", "-t", task.session_name], check=False)
        
        worktrees_dir = self.project_root.parent / f"worktrees/{self.spec_name}"
        if worktrees_dir.exists():
            for worktree in worktrees_dir.iterdir():
                self._run_cmd(["git", "worktree", "remove", str(worktree), "--force"], check=False)
        
        if self.state_file.exists():
            self.state_file.unlink()
        
        print("  ✓ Cleanup complete")


def print_analysis(plan: ExecutionPlan, all_tasks: dict[str, Task]):
    """Print analysis of the execution plan"""
    print("\n" + "="*60)
    print("TASK DEPENDENCY ANALYSIS")
    print("="*60)

    completed = sum(1 for t in all_tasks.values() if t.status == TaskStatus.COMPLETED)
    parent_tasks = sum(1 for t in all_tasks.values() if t.is_parent_task)
    pending = len(all_tasks) - completed

    print(f"\nSpec: {plan.spec_name}")
    print(f"Total Tasks: {len(all_tasks)} ({completed} completed, {pending} pending)")
    if parent_tasks > 0:
        print(f"Parent Tasks: {parent_tasks} (grouping headers - will be auto-skipped)")
    print(f"Execution Waves: {plan.total_waves}")

    if plan.waves:
        max_parallel = max(len(w) for w in plan.waves)
        avg_parallel = sum(len(w) for w in plan.waves) / len(plan.waves)
        print(f"Max Parallelism: {max_parallel} tasks")
        print(f"Avg Parallelism: {avg_parallel:.1f} tasks")

    # Show completed tasks
    if completed > 0:
        print("\n" + "-"*60)
        print("COMPLETED TASKS")
        print("-"*60)
        for task in all_tasks.values():
            if task.status == TaskStatus.COMPLETED:
                print(f"  ✅ Task {task.id}: {task.title[:50]}")

    # Show execution waves
    if plan.waves:
        print("\n" + "-"*60)
        print("EXECUTION WAVES (Pending Tasks)")
        print("-"*60)

        for wave_idx, task_ids in enumerate(plan.waves):
            print(f"\n🌊 Wave {wave_idx + 1} ({len(task_ids)} tasks in parallel):")
            for task_id in task_ids:
                task = plan.tasks[task_id]
                deps = ", ".join(task.dependencies) if task.dependencies else "None"
                parallel_marker = " (P)" if task.is_parallel else ""
                parent_marker = " [PARENT]" if task.is_parent_task else ""
                print(f"   ⬜ Task {task.id}{parallel_marker}{parent_marker}: {task.title[:45]}")
                if task.dependencies:
                    print(f"      └─ Depends on: {deps}")


class BackgroundLogRefresher:
    """Background thread that periodically refreshes log files and monitors running tasks.

    Features:
    - Refreshes log files for all running tasks
    - Sends Enter key if session is waiting for input
    - Auto-processes completed tasks (commit, merge, kill)
    - Auto-continues to next pending task when a slot opens up
    """

    def __init__(
        self,
        executor: ParallelExecutor,
        tasks: dict[str, Task],
        plan: Optional['ExecutionPlan'] = None,
        interval: int = 30,
        auto_process: bool = True,
        auto_enter: bool = True,
        auto_continue: bool = False
    ):
        self.executor = executor
        self.tasks = tasks
        self.plan = plan
        self.interval = interval
        self.auto_process = auto_process
        self.auto_enter = auto_enter
        self.auto_continue = auto_continue
        self._stop_event = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._processed_tasks: set[str] = set()
        self._lock = threading.Lock()

    def _get_next_eligible_task(self) -> Optional[Task]:
        """Get the next pending task that is eligible to run (dependencies satisfied).

        Returns the first pending task whose dependencies are all completed.
        Respects wave ordering - tasks from earlier waves are prioritized.
        """
        if not self.plan:
            return None

        # Go through waves in order
        for wave_task_ids in self.plan.waves:
            for task_id in wave_task_ids:
                task = self.tasks.get(task_id)
                if not task:
                    continue

                # Skip if not pending or is a parent task
                if task.status != TaskStatus.PENDING or task.is_parent_task:
                    continue

                # Check if all dependencies are completed
                deps_satisfied = all(
                    self.tasks.get(dep_id) and self.tasks[dep_id].status == TaskStatus.COMPLETED
                    for dep_id in task.dependencies
                )

                if deps_satisfied:
                    return task

        return None

    def _check_all_tasks_completed(self) -> bool:
        """Check if all tasks are completed and signal stop if so.

        Returns True if all tasks completed, False otherwise.
        """
        # Count task statuses
        completed_count = sum(
            1 for t in self.tasks.values()
            if t.status == TaskStatus.COMPLETED
        )
        pending_count = sum(
            1 for t in self.tasks.values()
            if t.status == TaskStatus.PENDING and not t.is_parent_task
        )
        running_count = self.executor._count_running(self.tasks)

        # All done if no pending and no running
        if pending_count == 0 and running_count == 0:
            total_tasks = len(self.tasks)
            parent_tasks = sum(1 for t in self.tasks.values() if t.is_parent_task)
            actual_tasks = total_tasks - parent_tasks

            print(f"\n🎉 [Monitor] All tasks completed! ({completed_count}/{actual_tasks} tasks)")
            print("   Stopping background monitor.")
            self._stop_event.set()
            return True

        return False

    def _check_deadlock_state(self) -> tuple[bool, list[str]]:
        """Check if we're in a deadlock state where pending tasks can never run.

        Returns (is_deadlocked, list of blocked task ids).

        A deadlock occurs when:
        - There are pending tasks
        - No tasks are running
        - All pending tasks have dependencies on tasks that are FAILED or STOPPED
        """
        if not self.plan:
            return False, []

        running_count = self.executor._count_running(self.tasks)
        if running_count > 0:
            return False, []  # Not deadlocked if tasks are running

        blocked_tasks = []
        for task in self.tasks.values():
            if task.status != TaskStatus.PENDING or task.is_parent_task:
                continue

            # Check if any dependency is in a terminal failed/stopped state
            has_unsatisfiable_dep = False
            for dep_id in task.dependencies:
                dep_task = self.tasks.get(dep_id)
                if dep_task and dep_task.status in (TaskStatus.FAILED, TaskStatus.STOPPED):
                    has_unsatisfiable_dep = True
                    break

            if has_unsatisfiable_dep:
                blocked_tasks.append(task.id)

        # Deadlocked if all pending tasks are blocked
        pending_count = sum(
            1 for t in self.tasks.values()
            if t.status == TaskStatus.PENDING and not t.is_parent_task
        )

        is_deadlocked = pending_count > 0 and len(blocked_tasks) == pending_count
        return is_deadlocked, blocked_tasks

    def _start_next_task(self) -> bool:
        """Start the next eligible pending task if there's an available slot.

        Returns True if a task was started, False otherwise.
        """
        # Check if we have available slots
        running_count = self.executor._count_running(self.tasks)
        if running_count >= self.executor.max_parallel:
            return False

        # Get next eligible task
        next_task = self._get_next_eligible_task()
        if not next_task:
            return False

        # Start the task
        print(f"\n🚀 [Monitor] Starting next task {next_task.id}: {next_task.title[:50]}...")
        self.executor.start_task(next_task)
        if self.plan:
            self.executor.save_state(self.plan)
        print(f"🚀 [Monitor] Task {next_task.id} started in session {next_task.session_name}")
        return True

    def _refresh_loop(self):
        """Main loop that refreshes logs and monitors tasks periodically."""
        while not self._stop_event.wait(self.interval):
            try:
                with self._lock:
                    # Capture tasks that were RUNNING before status refresh
                    # (needed to detect tasks that just stopped)
                    previously_running = {
                        task_id for task_id, task in self.tasks.items()
                        if task.status == TaskStatus.RUNNING
                    }

                    # Refresh session status - this may change RUNNING -> STOPPED
                    self.executor.refresh_session_status(self.tasks)

                    # Update all running task logs
                    self.executor.update_all_running_logs(self.tasks)

                    # Process tasks that are RUNNING or just became STOPPED
                    for task_id, task in self.tasks.items():
                        # Skip if not relevant
                        was_running = task_id in previously_running
                        is_running = task.status == TaskStatus.RUNNING
                        just_stopped = was_running and task.status == TaskStatus.STOPPED

                        if not is_running and not just_stopped:
                            continue

                        # Auto-send Enter if waiting for input (only for running tasks)
                        if is_running and self.auto_enter:
                            if self.executor.send_enter_if_waiting(task):
                                print(f"\n⏎ [Monitor] Sent Enter to task {task.id} (was waiting)")

                        # Check for completion and auto-process
                        # For running tasks: check if completed
                        # For just-stopped tasks: process them (session ended)
                        if self.auto_process and task_id not in self._processed_tasks:
                            should_process = just_stopped or (is_running and self.executor.is_task_completed(task))
                            if should_process:
                                print(f"\n✅ [Monitor] Task {task.id} {'stopped' if just_stopped else 'completed'}, processing...")
                                self.executor.process_completed_task(task)
                                self._processed_tasks.add(task_id)
                                if self.plan:
                                    self.executor.save_state(self.plan)
                                print(f"✅ [Monitor] Task {task.id} processed (committed, merged, killed)")

                                # Auto-continue: start next pending task if enabled
                                if self.auto_continue:
                                    self._start_next_task()

                    # Also check for auto-continue even without completed task
                    # (in case slots opened up from manual kills or other reasons)
                    if self.auto_continue:
                        # Keep starting tasks until no more slots or no more eligible tasks
                        while self._start_next_task():
                            pass

                        # Check for deadlock state
                        is_deadlocked, blocked_tasks = self._check_deadlock_state()
                        if is_deadlocked:
                            print(f"\n🚨 [Monitor] DEADLOCK DETECTED: {len(blocked_tasks)} tasks blocked due to failed dependencies")
                            print(f"   Blocked tasks: {', '.join(blocked_tasks)}")
                            print("   Manual intervention required. Stopping auto-continue.")
                            self._stop_event.set()  # Signal to stop the monitor

                    # Check if all tasks are completed (auto-exit when done)
                    self._check_all_tasks_completed()

            except Exception as e:
                # Log errors but don't crash the background thread
                print(f"\n⚠️ [Monitor] Error: {e}")

    def start(self):
        """Start the background monitoring thread."""
        if self._thread is not None and self._thread.is_alive():
            return  # Already running

        self._stop_event.clear()

        # Do immediate refresh before starting background thread
        self._do_immediate_refresh()

        self._thread = threading.Thread(target=self._refresh_loop, daemon=True)
        self._thread.start()
        features = []
        if self.auto_enter:
            features.append("auto-enter")
        if self.auto_process:
            features.append("auto-process")
        if self.auto_continue:
            features.append("auto-continue")
        feature_str = f" [{', '.join(features)}]" if features else ""
        print(f"📝 Background monitor started (interval: {self.interval}s){feature_str}")

    def _do_immediate_refresh(self):
        """Do an immediate refresh of all running task logs and start initial tasks if auto_continue."""
        try:
            self.executor.refresh_session_status(self.tasks)
            self.executor.update_all_running_logs(self.tasks)

            # If auto_continue is enabled, start eligible tasks immediately
            if self.auto_continue:
                started_count = 0
                while self._start_next_task():
                    started_count += 1
                if started_count > 0:
                    print(f"🚀 [Monitor] Started {started_count} initial task(s)")
        except Exception as e:
            print(f"⚠️ [Monitor] Initial refresh error: {e}")

    def stop(self):
        """Stop the background monitoring thread."""
        if self._thread is None:
            return

        self._stop_event.set()
        self._thread.join(timeout=5)
        self._thread = None
        print("📝 Background monitor stopped")

    def is_running(self) -> bool:
        """Check if the background monitor is running."""
        return self._thread is not None and self._thread.is_alive()

    def get_processed_tasks(self) -> set[str]:
        """Get the set of task IDs that have been processed."""
        with self._lock:
            return self._processed_tasks.copy()


# Global log refresher instance
_log_refresher: Optional[BackgroundLogRefresher] = None


def start_log_refresher(
    executor: ParallelExecutor,
    tasks: dict[str, Task],
    plan: Optional['ExecutionPlan'] = None,
    interval: int = 30,
    auto_process: bool = True,
    auto_enter: bool = True,
    auto_continue: bool = False
):
    """Start the global background monitor.

    Args:
        executor: The ParallelExecutor instance
        tasks: Dict of task_id -> Task
        plan: ExecutionPlan for saving state on completion
        interval: Refresh interval in seconds (default: 30)
        auto_process: Auto-commit/merge/kill on completion (default: True)
        auto_enter: Auto-send Enter when waiting for input (default: True)
        auto_continue: Auto-start next pending task when a slot opens (default: False)
    """
    global _log_refresher
    if _log_refresher is not None and _log_refresher.is_running():
        _log_refresher.stop()
    _log_refresher = BackgroundLogRefresher(
        executor, tasks, plan, interval, auto_process, auto_enter, auto_continue
    )
    _log_refresher.start()


def stop_log_refresher():
    """Stop the global background log refresher."""
    global _log_refresher
    if _log_refresher is not None:
        _log_refresher.stop()
        _log_refresher = None


################################################################################
# Master-Worker BGR Architecture
#
# - Master: spawns up to max_parallel Workers, waits for all to exit
# - Worker: handles one task at a time, claims next task when done, exits when no more tasks
# - Each Worker has its own PID file and task file
# - File locking prevents race conditions when claiming tasks
################################################################################

import fcntl
import signal

MAX_WORKERS = 16  # Maximum possible workers


def get_master_pid_file(spec_name: str, project_root: Path = Path(".")) -> Path:
    """Get the path to the master PID file."""
    return project_root / ".kiro" / "specs" / spec_name / ".master.pid"


def get_worker_pid_file(spec_name: str, worker_id: int, project_root: Path = Path(".")) -> Path:
    """Get the path to a worker's PID file."""
    return project_root / ".kiro" / "specs" / spec_name / f".worker-{worker_id}.pid"


def get_worker_task_file(spec_name: str, worker_id: int, project_root: Path = Path(".")) -> Path:
    """Get the path to a worker's current task file."""
    return project_root / ".kiro" / "specs" / spec_name / f".worker-{worker_id}.task"


def get_task_lock_file(spec_name: str, project_root: Path = Path(".")) -> Path:
    """Get the path to the task claiming lock file."""
    return project_root / ".kiro" / "specs" / spec_name / ".task_lock"


def get_completed_tasks_file(spec_name: str, project_root: Path = Path(".")) -> Path:
    """Get the path to the completed tasks log file (for watch display)."""
    return project_root / ".kiro" / "specs" / spec_name / ".completed_tasks.json"


def is_process_running(pid: int) -> bool:
    """Check if a process with given PID is running."""
    try:
        os.kill(pid, 0)
        return True
    except (ProcessLookupError, PermissionError):
        return False


def get_all_claimed_tasks(spec_name: str, project_root: Path = Path(".")) -> dict[str, int]:
    """Get all currently claimed tasks and their worker IDs.

    Returns dict mapping task_id -> worker_id.
    """
    claimed = {}
    for worker_id in range(MAX_WORKERS):
        task_file = get_worker_task_file(spec_name, worker_id, project_root)
        pid_file = get_worker_pid_file(spec_name, worker_id, project_root)

        if task_file.exists() and pid_file.exists():
            try:
                pid = int(pid_file.read_text().strip())
                if is_process_running(pid):
                    task_id = task_file.read_text().strip()
                    if task_id:
                        claimed[task_id] = worker_id
            except (ValueError, OSError):
                pass
    return claimed


def get_active_workers(spec_name: str, project_root: Path = Path(".")) -> list[tuple[int, int, str]]:
    """Get all active workers.

    Returns list of (worker_id, pid, task_id) tuples.
    """
    workers = []
    for worker_id in range(MAX_WORKERS):
        pid_file = get_worker_pid_file(spec_name, worker_id, project_root)
        task_file = get_worker_task_file(spec_name, worker_id, project_root)

        if pid_file.exists():
            try:
                pid = int(pid_file.read_text().strip())
                if is_process_running(pid):
                    task_id = ""
                    if task_file.exists():
                        task_id = task_file.read_text().strip()
                    workers.append((worker_id, pid, task_id))
            except (ValueError, OSError):
                pass
    return workers


def claim_next_task(
    spec_name: str,
    worker_id: int,
    executor: ParallelExecutor,
    project_root: Path = Path(".")
) -> Optional[Task]:
    """Atomically claim the next eligible task.

    Uses file locking to prevent race conditions between workers.
    Returns the claimed Task or None if no tasks available.
    """
    lock_file = get_task_lock_file(spec_name, project_root)
    lock_file.parent.mkdir(parents=True, exist_ok=True)

    # Open lock file (create if needed)
    with open(lock_file, 'w') as f:
        try:
            fcntl.flock(f, fcntl.LOCK_EX)  # Exclusive lock

            # Re-parse tasks to get fresh state
            parser = TasksParser()
            tasks = parser.parse(spec_name)
            saved_state = executor.load_state()
            apply_saved_state(tasks, saved_state, spec_name)
            executor.refresh_session_status(tasks)

            # Analyze dependencies
            analyzer = DependencyAnalyzer()
            plan = analyzer.analyze(tasks)
            plan.spec_name = spec_name

            # Get currently claimed tasks
            claimed_tasks = get_all_claimed_tasks(spec_name, project_root)

            # Find next eligible task
            for wave_task_ids in plan.waves:
                for task_id in wave_task_ids:
                    task = tasks.get(task_id)
                    if not task:
                        continue

                    # Skip if already running or completed
                    if task.status in (TaskStatus.RUNNING, TaskStatus.COMPLETED):
                        continue

                    # Skip if not pending or is parent task
                    if task.status != TaskStatus.PENDING or task.is_parent_task:
                        continue

                    # Skip if already claimed by another worker
                    if task_id in claimed_tasks:
                        continue

                    # Also check if session already exists (in case state wasn't saved)
                    safe_id = task_id.replace('.', '-')
                    session_name = f"{spec_name}-task-{safe_id}"
                    result = subprocess.run(
                        ["tmux", "has-session", "-t", session_name],
                        capture_output=True, check=False
                    )
                    if result.returncode == 0:
                        # Session exists - skip this task
                        continue

                    # Check dependencies are satisfied
                    deps_satisfied = all(
                        tasks.get(dep_id) and tasks[dep_id].status == TaskStatus.COMPLETED
                        for dep_id in task.dependencies
                    )

                    if deps_satisfied:
                        # Claim this task
                        task_file = get_worker_task_file(spec_name, worker_id, project_root)
                        task_file.write_text(task_id)
                        return task

            return None

        finally:
            fcntl.flock(f, fcntl.LOCK_UN)  # Release lock


def claim_running_task(
    spec_name: str,
    worker_id: int,
    executor: ParallelExecutor,
    project_root: Path = Path(".")
) -> Optional[Task]:
    """Claim an already-running task that has no worker monitoring it.

    This is used to resume monitoring tasks after worker restart.
    Also resets orphaned tasks (marked running but session gone) to pending.
    Returns the claimed Task or None if no orphaned running tasks.
    """
    lock_file = get_task_lock_file(spec_name, project_root)
    lock_file.parent.mkdir(parents=True, exist_ok=True)

    with open(lock_file, 'w') as f:
        try:
            fcntl.flock(f, fcntl.LOCK_EX)

            # Re-parse tasks to get fresh state
            parser = TasksParser()
            tasks = parser.parse(spec_name)
            saved_state = executor.load_state()
            apply_saved_state(tasks, saved_state, spec_name)

            # Get currently claimed tasks
            claimed_tasks = get_all_claimed_tasks(spec_name, project_root)

            # Track if we need to save state (for resetting orphaned tasks)
            state_changed = False

            # Find running tasks not claimed by any worker
            for task_id, task in tasks.items():
                if task.status != TaskStatus.RUNNING:
                    continue

                # Skip if already claimed by another worker
                if task_id in claimed_tasks:
                    continue

                # Check if session actually exists
                session_exists = False
                if task.session_name:
                    result = subprocess.run(
                        ["tmux", "has-session", "-t", task.session_name],
                        capture_output=True, check=False
                    )
                    session_exists = (result.returncode == 0)

                if not session_exists:
                    # Session doesn't exist - reset task to pending
                    print(f"[claim_running_task] Task {task_id} marked running but session gone, resetting to pending")
                    task.status = TaskStatus.PENDING
                    task.session_name = ""
                    task.start_time = 0.0
                    state_changed = True
                    continue

                # Session exists and not claimed - claim it
                task_file = get_worker_task_file(spec_name, worker_id, project_root)
                task_file.write_text(task_id)

                # Save state if we made changes
                if state_changed:
                    analyzer = DependencyAnalyzer()
                    plan = analyzer.analyze(tasks)
                    plan.spec_name = spec_name
                    executor.save_state(plan)

                return task

            # Save state if we reset any orphaned tasks
            if state_changed:
                analyzer = DependencyAnalyzer()
                plan = analyzer.analyze(tasks)
                plan.spec_name = spec_name
                executor.save_state(plan)

            return None

        finally:
            fcntl.flock(f, fcntl.LOCK_UN)


def release_task(spec_name: str, worker_id: int, project_root: Path = Path(".")):
    """Release the current task claim."""
    task_file = get_worker_task_file(spec_name, worker_id, project_root)
    if task_file.exists():
        task_file.unlink()


def recover_orphaned_sessions(spec_name: str, executor: ParallelExecutor, project_root: Path = Path(".")):
    """Find and recover sessions that are running but not tracked in state.

    This can happen if a worker crashed after starting a task but before saving state.
    Returns list of recovered (task_id, session_name) tuples.
    """
    recovered = []

    # Get all tmux sessions for this spec
    result = subprocess.run(
        ["tmux", "list-sessions", "-F", "#{session_name}"],
        capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        return recovered

    session_prefix = f"{spec_name}-task-"
    for line in result.stdout.strip().split('\n'):
        session_name = line.strip()
        if not session_name.startswith(session_prefix):
            continue

        # Extract task ID from session name
        task_suffix = session_name[len(session_prefix):]
        task_id = task_suffix.replace('-', '.')

        # Check if this task is tracked in state
        saved_state = executor.load_state()
        task_state = saved_state.get("tasks", {}).get(task_id, {})

        if task_state.get("status") != "running" or not task_state.get("session_name"):
            # Session exists but not tracked - recover it
            print(f"[Recovery] Found orphaned session for task {task_id}: {session_name}")

            # Update state to track this session
            if "tasks" not in saved_state:
                saved_state["tasks"] = {}
            if task_id not in saved_state["tasks"]:
                saved_state["tasks"][task_id] = {}

            saved_state["tasks"][task_id]["status"] = "running"
            saved_state["tasks"][task_id]["session_name"] = session_name
            if not saved_state["tasks"][task_id].get("start_time"):
                saved_state["tasks"][task_id]["start_time"] = time.time()

            executor.state_file.write_text(json.dumps(saved_state, indent=2))
            recovered.append((task_id, session_name))

    return recovered


def log_completed_task(spec_name: str, task: Task, duration: int, project_root: Path = Path(".")):
    """Log a completed task for watch display."""
    completed_file = get_completed_tasks_file(spec_name, project_root)

    # Read existing entries
    entries = []
    if completed_file.exists():
        try:
            entries = json.loads(completed_file.read_text())
        except (json.JSONDecodeError, OSError):
            entries = []

    # Add new entry
    entries.append({
        "task_id": task.id,
        "title": task.title,
        "duration": duration,
        "completed_at": datetime.now().isoformat()
    })

    # Keep last 50 entries
    entries = entries[-50:]

    completed_file.write_text(json.dumps(entries, indent=2))


def start_worker_bgr(
    spec_name: str,
    worker_id: int,
    interval: int = 30,
    project_root: Path = Path(".")
) -> Optional[int]:
    """Start a worker BGR as a detached background process.

    Returns the PID of the spawned process, or None if failed.
    """
    import sys

    script_path = Path(__file__).resolve()
    cmd = [
        sys.executable, str(script_path),
        "worker", spec_name,
        "--worker-id", str(worker_id),
        "--interval", str(interval),
    ]

    # Get log file path
    logs_dir = project_root / ".kiro" / "specs" / spec_name / "logs"
    logs_dir.mkdir(parents=True, exist_ok=True)
    worker_log = logs_dir / f"worker-{worker_id}.log"

    # Spawn detached process
    with open(worker_log, "a") as log_file:
        log_file.write(f"\n{'='*60}\n")
        log_file.write(f"Worker {worker_id} started at {datetime.now().isoformat()}\n")
        log_file.write(f"{'='*60}\n")
        log_file.flush()

        process = subprocess.Popen(
            cmd,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            start_new_session=True,
            cwd=str(project_root)
        )

    # Save PID
    pid_file = get_worker_pid_file(spec_name, worker_id, project_root)
    pid_file.write_text(str(process.pid))

    return process.pid


def stop_worker_bgr(spec_name: str, worker_id: int, project_root: Path = Path(".")) -> bool:
    """Stop a specific worker BGR.

    Returns True if worker was stopped, False if not running.
    """
    pid_file = get_worker_pid_file(spec_name, worker_id, project_root)
    task_file = get_worker_task_file(spec_name, worker_id, project_root)

    if not pid_file.exists():
        return False

    try:
        pid = int(pid_file.read_text().strip())
        os.kill(pid, signal.SIGTERM)
        time.sleep(0.5)  # Give it time to clean up

        # Clean up files
        if pid_file.exists():
            pid_file.unlink()
        if task_file.exists():
            task_file.unlink()
        return True
    except (ProcessLookupError, ValueError):
        # Process already dead
        if pid_file.exists():
            pid_file.unlink()
        if task_file.exists():
            task_file.unlink()
        return False


def stop_task_by_id(spec_name: str, task_id: str, project_root: Path = Path(".")) -> bool:
    """Stop a specific task and its worker BGR.

    Does NOT clean worktree or commit - preserves work for resume.
    Returns True if task was stopped, False if not found.
    """
    # Find which worker has this task
    claimed_tasks = get_all_claimed_tasks(spec_name, project_root)

    if task_id not in claimed_tasks:
        # Task not currently running - check if session exists anyway
        executor = ParallelExecutor(spec_name=spec_name, project_root=str(project_root))
        safe_id = task_id.replace('.', '-')
        session_name = f"{spec_name}-task-{safe_id}"

        # Try to kill session if it exists
        result = subprocess.run(
            ["tmux", "kill-session", "-t", session_name],
            capture_output=True, check=False
        )
        return result.returncode == 0

    worker_id = claimed_tasks[task_id]

    # Get task's session name
    safe_id = task_id.replace('.', '-')
    session_name = f"{spec_name}-task-{safe_id}"

    # Kill the tmux session first
    subprocess.run(
        ["tmux", "kill-session", "-t", session_name],
        capture_output=True, check=False
    )

    # Stop the worker
    return stop_worker_bgr(spec_name, worker_id, project_root)


def stop_master(spec_name: str, project_root: Path = Path(".")) -> bool:
    """Stop the master process and all workers.

    Returns True if master was stopped.
    """
    # Stop all workers first
    for worker_id in range(MAX_WORKERS):
        stop_worker_bgr(spec_name, worker_id, project_root)

    # Stop master
    pid_file = get_master_pid_file(spec_name, project_root)
    if not pid_file.exists():
        return False

    try:
        pid = int(pid_file.read_text().strip())
        os.kill(pid, signal.SIGTERM)
        pid_file.unlink()
        return True
    except (ProcessLookupError, ValueError):
        if pid_file.exists():
            pid_file.unlink()
        return False


def is_master_running(spec_name: str, project_root: Path = Path(".")) -> tuple[bool, int]:
    """Check if master process is running.

    Returns (is_running, pid).
    """
    pid_file = get_master_pid_file(spec_name, project_root)
    if not pid_file.exists():
        return False, 0

    try:
        pid = int(pid_file.read_text().strip())
        if is_process_running(pid):
            return True, pid
        return False, 0
    except (ValueError, OSError):
        return False, 0


def start_master(
    spec_name: str,
    max_parallel: int = 4,
    interval: int = 30,
    project_root: Path = Path(".")
) -> int:
    """Start the master process as a detached background process.

    Returns the PID of the spawned process.
    """
    import sys

    script_path = Path(__file__).resolve()
    cmd = [
        sys.executable, str(script_path),
        "master", spec_name,
        "--max-parallel", str(max_parallel),
        "--interval", str(interval),
    ]

    # Get log file path
    logs_dir = project_root / ".kiro" / "specs" / spec_name / "logs"
    logs_dir.mkdir(parents=True, exist_ok=True)
    master_log = logs_dir / "master.log"

    # Spawn detached process
    with open(master_log, "a") as log_file:
        log_file.write(f"\n{'='*60}\n")
        log_file.write(f"Master started at {datetime.now().isoformat()}\n")
        log_file.write(f"Max parallel: {max_parallel}, Interval: {interval}s\n")
        log_file.write(f"{'='*60}\n")
        log_file.flush()

        process = subprocess.Popen(
            cmd,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            start_new_session=True,
            cwd=str(project_root)
        )

    # Save PID
    pid_file = get_master_pid_file(spec_name, project_root)
    pid_file.write_text(str(process.pid))

    return process.pid


# Legacy compatibility aliases
def get_bgr_pid_file(spec_name: str, project_root: Path = Path(".")) -> Path:
    """Legacy: Get the path to the BGR PID file (now master)."""
    return get_master_pid_file(spec_name, project_root)


def is_bgr_running(spec_name: str, project_root: Path = Path(".")) -> tuple[bool, int]:
    """Legacy: Check if BGR (master) is running."""
    return is_master_running(spec_name, project_root)


def generate_mermaid(plan: ExecutionPlan) -> str:
    """Generate Mermaid diagram of dependencies"""
    lines = ["```mermaid", "graph TD"]
    
    for wave_idx, task_ids in enumerate(plan.waves):
        lines.append(f"    subgraph Wave{wave_idx + 1}[\"Wave {wave_idx + 1}\"]")
        for task_id in task_ids:
            task = plan.tasks[task_id]
            safe_id = task_id.replace('.', '_')
            safe_title = task.title[:25].replace('"', "'")
            lines.append(f'        T{safe_id}["{task_id}: {safe_title}"]')
        lines.append("    end")
    
    for task in plan.tasks.values():
        if task.status == TaskStatus.COMPLETED:
            continue
        for dep in task.dependencies:
            if dep in plan.tasks and plan.tasks[dep].status != TaskStatus.COMPLETED:
                safe_from = dep.replace('.', '_')
                safe_to = task.id.replace('.', '_')
                lines.append(f"    T{safe_from} --> T{safe_to}")
    
    lines.append("```")
    return "\n".join(lines)


def apply_saved_state(tasks: dict[str, Task], saved_state: Optional[dict], spec_name: str):
    """Apply saved running state to tasks to avoid duplicate starts"""
    if not saved_state:
        return

    if saved_state.get("spec_name") and saved_state["spec_name"] != spec_name:
        return

    for task_id, task_state in saved_state.get("tasks", {}).items():
        if task_id not in tasks:
            continue

        saved_status = task_state.get("status")
        if saved_status == TaskStatus.RUNNING.value:
            if tasks[task_id].status != TaskStatus.COMPLETED:
                tasks[task_id].status = TaskStatus.RUNNING
                tasks[task_id].session_name = task_state.get("session_name", "")
        elif saved_status == TaskStatus.STOPPED.value:
            tasks[task_id].status = TaskStatus.STOPPED
            tasks[task_id].session_name = task_state.get("session_name", "")
        
        start_time = task_state.get("start_time")
        if start_time is not None:
            tasks[task_id].start_time = start_time


def summarize_wave(tasks: dict[str, Task], wave_task_ids: list[str]) -> dict:
    """Summarize statuses for a wave"""
    summary = {
        "pending": 0,
        "running": 0,
        "stopped": 0,
        "completed": 0,
        "failed": 0,
        "blocked": 0
    }
    details = []
    for tid in wave_task_ids:
        task = tasks.get(tid)
        if not task:
            continue
        summary[task.status.value] = summary.get(task.status.value, 0) + 1
        details.append(task)
    return {"summary": summary, "details": details}


def get_session_activity_summary(session_name: str, max_chars: int = 60) -> str:
    """Get a brief summary of what's happening in a tmux session.

    Analyzes the last few lines of the session output to determine
    the current activity (thinking, reading, writing, etc.)

    Returns a short string describing the current activity.
    """
    if not session_name:
        return ""

    # Capture last 30 lines from session
    result = subprocess.run(
        ["tmux", "capture-pane", "-t", session_name, "-p", "-S", "-30"],
        capture_output=True, text=True, check=False
    )

    if result.returncode != 0 or not result.stdout:
        return "..."

    output = result.stdout
    lines = output.strip().split('\n')
    last_lines = '\n'.join(lines[-15:]) if len(lines) >= 15 else output

    # Priority-ordered activity detection patterns
    activity_patterns = [
        # Active thinking/processing
        (r'Thinking[…\.]+\s*\(.*?(\d+m\s*\d+s|\d+s)', lambda m: f"🤔 Thinking ({m.group(1)})"),
        (r'Thinking[…\.]+', lambda m: "🤔 Thinking..."),

        # Tool execution indicators
        (r'⏺\s+Read\s+(.+?)(?:\s|$)', lambda m: f"📖 Reading {_truncate_path(m.group(1), 35)}"),
        (r'⏺\s+Write\s+(.+?)(?:\s|$)', lambda m: f"✏️ Writing {_truncate_path(m.group(1), 35)}"),
        (r'⏺\s+Edit\s+(.+?)(?:\s|$)', lambda m: f"📝 Editing {_truncate_path(m.group(1), 35)}"),
        (r'⏺\s+Bash\s*[:\(]?\s*(.+?)(?:\)|$)', lambda m: f"💻 Running: {m.group(1)[:30]}"),
        (r'⏺\s+Glob\s+(.+?)(?:\s|$)', lambda m: f"🔍 Globbing {m.group(1)[:30]}"),
        (r'⏺\s+Grep\s+(.+?)(?:\s|$)', lambda m: f"🔍 Searching: {m.group(1)[:30]}"),
        (r'⏺\s+Task\s+(.+?)(?:\s|$)', lambda m: f"📋 Agent: {m.group(1)[:35]}"),
        (r'⏺\s+TodoWrite', lambda m: "📋 Updating todos"),
        (r'⏺\s+WebSearch\s+(.+?)(?:\s|$)', lambda m: f"🌐 Searching: {m.group(1)[:30]}"),
        (r'⏺\s+WebFetch\s+(.+?)(?:\s|$)', lambda m: f"🌐 Fetching: {m.group(1)[:30]}"),
        (r'⏺\s+mcp__serena__(\w+)', lambda m: f"🔧 Serena: {m.group(1)}"),
        (r'⏺\s+mcp__(\w+)__(\w+)', lambda m: f"🔧 {m.group(1)}: {m.group(2)}"),
        (r'⏺\s+(\w+)', lambda m: f"⚙️ {m.group(1)}"),

        # Coalescing/processing
        (r'Coalescing', lambda m: "⚡ Processing results..."),

        # Completion indicators
        (r'⎿\s*Done\s*\((\d+)\s*tool', lambda m: f"✅ Done ({m.group(1)} tools)"),
        (r'Done\s*\((\d+)\s*tool', lambda m: f"✅ Done ({m.group(1)} tools)"),

        # Waiting states
        (r'is running[…\.]+', lambda m: "⏳ Waiting for approval..."),
        (r'0\s*tokens', lambda m: "⏳ Waiting to start..."),
        (r'\[Y/n\]|\[y/N\]|\(y/n\)', lambda m: "❓ Waiting for confirmation..."),

        # Token usage indicator (shows progress)
        (r'(\d{4,})\s*tokens', lambda m: f"📊 {int(m.group(1)):,} tokens used"),
    ]

    # Try each pattern in priority order
    for pattern, formatter in activity_patterns:
        match = re.search(pattern, last_lines, re.IGNORECASE)
        if match:
            try:
                result_str = formatter(match)
                if len(result_str) > max_chars:
                    result_str = result_str[:max_chars-3] + "..."
                return result_str
            except Exception:
                continue

    # Fallback: show last non-empty line (truncated)
    for line in reversed(lines):
        stripped = line.strip()
        if stripped and not stripped.startswith('>') and len(stripped) > 5:
            # Clean up the line
            clean_line = re.sub(r'\x1b\[[0-9;]*m', '', stripped)  # Remove ANSI codes
            if len(clean_line) > max_chars:
                clean_line = clean_line[:max_chars-3] + "..."
            return clean_line

    return "..."


def _truncate_path(path: str, max_len: int) -> str:
    """Truncate a file path intelligently, keeping the filename."""
    if len(path) <= max_len:
        return path

    # Try to keep the filename
    parts = path.split('/')
    if len(parts) > 1:
        filename = parts[-1]
        if len(filename) <= max_len - 4:
            return ".../" + filename

    return path[:max_len-3] + "..."


def watch_wave(executor: ParallelExecutor, plan: ExecutionPlan, wave_idx: int, interval: int, auto_continue: bool = False):
    """Watch task status - DISPLAY ONLY, no actions.

    This function is purely for monitoring and displaying status.
    All actions (auto-enter, auto-process, task starting) are handled by BGR.
    Status refresh is done here to show accurate session state.

    Args:
        executor: The ParallelExecutor instance
        plan: The ExecutionPlan
        wave_idx: Wave index to start watching from
        interval: Display refresh interval in seconds
        auto_continue: If True, watch all tasks globally; if False, watch only wave tasks
    """
    # Track all task IDs we're watching (dynamically updated in auto_continue mode)
    watched_task_ids = set(plan.waves[wave_idx])
    print(f"\nWatching {'all tasks' if auto_continue else f'Wave {wave_idx + 1}'} (interval: {interval}s). Press Ctrl+C to stop.")
    print("Actions handled by BGR. This display auto-refreshes session status.")
    print(f"Log files in: {executor.get_logs_dir()}\n")

    start_time = time.time()
    printed_lines = 0

    # Re-load tasks and state periodically to catch BGR changes
    kiro_dir = ".kiro"
    parser_obj = TasksParser(kiro_dir)

    try:
        while True:
            # Refresh: re-parse tasks, apply saved state, and refresh session status
            # This catches changes made by BGR (completions, new task starts, etc.)
            try:
                fresh_tasks = parser_obj.parse(plan.spec_name)
                saved_state = executor.load_state()
                apply_saved_state(fresh_tasks, saved_state, plan.spec_name)
                executor.refresh_session_status(fresh_tasks)

                # Update plan.tasks with fresh data
                for task_id, fresh_task in fresh_tasks.items():
                    if task_id in plan.tasks:
                        plan.tasks[task_id].status = fresh_task.status
                        plan.tasks[task_id].session_name = fresh_task.session_name
                        plan.tasks[task_id].start_time = fresh_task.start_time
            except Exception as e:
                pass  # Continue with existing data if refresh fails

            # In auto_continue mode, update watched_task_ids to include all running/completed tasks
            if auto_continue:
                for task in plan.tasks.values():
                    if task.status in (TaskStatus.RUNNING, TaskStatus.COMPLETED):
                        watched_task_ids.add(task.id)

            # Collect tasks for display
            running_tasks = [
                plan.tasks[tid] for tid in watched_task_ids
                if plan.tasks.get(tid) and plan.tasks[tid].status == TaskStatus.RUNNING
            ]
            pending_tasks = [
                plan.tasks[tid] for tid in watched_task_ids
                if plan.tasks.get(tid) and plan.tasks[tid].status == TaskStatus.PENDING
            ]
            completed_tasks = [
                plan.tasks[tid] for tid in watched_task_ids
                if plan.tasks.get(tid) and plan.tasks[tid].status == TaskStatus.COMPLETED
            ]

            # Count global pending (for auto-continue mode)
            global_pending = sum(
                1 for task in plan.tasks.values()
                if task.status == TaskStatus.PENDING and not task.is_parent_task
            )

            elapsed_total = int(time.time() - start_time)
            mins, secs = divmod(elapsed_total, 60)
            hours, mins = divmod(mins, 60)
            elapsed_str = f"{hours:02d}:{mins:02d}:{secs:02d}"

            if auto_continue:
                header = f"Auto-Continue | {elapsed_str} | ✅{len(completed_tasks)} 🔄{len(running_tasks)} ⏳{global_pending} total pending"
            else:
                header = f"Wave {wave_idx + 1} | {elapsed_str} | ✅{len(completed_tasks)} 🔄{len(running_tasks)} ⏳{len(pending_tasks)}"

            lines: list[str] = [header]

            if not running_tasks:
                if auto_continue:
                    # In auto-continue mode, exit only if no more pending tasks globally
                    if global_pending == 0:
                        lines.append(f"All tasks completed! Total: {len(completed_tasks)} completed. Exiting watch.")
                        line_count = max(printed_lines, len(lines))
                        lines.extend([""] * (line_count - len(lines)))
                        if printed_lines:
                            print(f"\033[{printed_lines}F", end="")
                        for ln in lines:
                            print(f"\r{ln}\033[K")
                        print()
                        return
                    # Has pending but no running - waiting for BGR to start tasks or manual intervention
                else:
                    # In wave mode, exit if no running and no pending in this wave
                    if not pending_tasks:
                        lines.append(f"Wave {wave_idx + 1} complete! {len(completed_tasks)} tasks done. Exiting watch.")
                        line_count = max(printed_lines, len(lines))
                        lines.extend([""] * (line_count - len(lines)))
                        if printed_lines:
                            print(f"\033[{printed_lines}F", end="")
                        for ln in lines:
                            print(f"\r{ln}\033[K")
                        print()
                        return

            # Show active workers info
            workers = get_active_workers(plan.spec_name)
            lines.append(f"Workers: {len(workers)} active")
            lines.append("")

            # Show running tasks
            if running_tasks:
                lines.append("Running Tasks:")
                for task in running_tasks:
                    elapsed = max(0, int(time.time() - task.start_time)) if task.start_time else 0
                    bar_width = 20
                    ratio = (elapsed % 60) / 60 if elapsed else 0
                    filled = int(bar_width * ratio)
                    bar = "[" + "#" * filled + "." * (bar_width - filled) + "]"
                    mins_t, secs_t = divmod(elapsed, 60)
                    hours_t, mins_t = divmod(mins_t, 60)
                    elapsed_task = f"{hours_t:02d}:{mins_t:02d}:{secs_t:02d}"
                    title = task.title[:35]

                    # Get activity summary for this session
                    activity = get_session_activity_summary(task.session_name, max_chars=50)

                    lines.append(f"{task.id:<6} {bar} {elapsed_task} | {title:<35}")
                    lines.append(f"       └─ {activity}")
            else:
                lines.append("No running tasks")

            # Show recently completed tasks from the log file
            lines.append("")
            completed_file = get_completed_tasks_file(plan.spec_name)
            if completed_file.exists():
                try:
                    entries = json.loads(completed_file.read_text())
                    if entries:
                        lines.append("Completed This Session:")
                        for entry in entries[-5:]:  # Show last 5
                            mins_c, secs_c = divmod(entry['duration'], 60)
                            title_c = entry['title'][:40]
                            lines.append(f"  ✅ {entry['task_id']} {title_c} ({mins_c:02d}:{secs_c:02d})")
                except (json.JSONDecodeError, OSError):
                    pass

            # Keep output to fixed number of lines
            line_count = max(printed_lines, len(lines))
            lines.extend([""] * (line_count - len(lines)))

            if printed_lines:
                print(f"\033[{printed_lines}F", end="")
            for ln in lines:
                print(f"\r{ln}\033[K")
            printed_lines = line_count

            time.sleep(interval)
    except KeyboardInterrupt:
        print("\n\nWatch stopped. (Display only - no state changes made)")


def main():
    parser = argparse.ArgumentParser(
        description="Parallel Task Orchestrator for cc-sdd specs"
    )
    subparsers = parser.add_subparsers(dest="command", help="Commands")
    
    # Analyze command
    analyze_parser = subparsers.add_parser("analyze", help="Analyze task dependencies")
    analyze_parser.add_argument("spec_name", help="Name of the spec")
    analyze_parser.add_argument("--mermaid", action="store_true", help="Output Mermaid diagram")
    analyze_parser.add_argument("--kiro-dir", default=".kiro", help="Path to .kiro directory")
    
    # Run command
    run_parser = subparsers.add_parser("run", help="Execute tasks in parallel")
    run_parser.add_argument("spec_name", help="Name of the spec")
    run_parser.add_argument("--max-parallel", type=int, default=4, help="Max concurrent tasks")
    run_parser.add_argument("--wave", type=int, help="Run specific wave only")
    run_parser.add_argument("--dry-run", action="store_true", help="Show plan without executing")
    run_parser.add_argument("--no-par", action="store_true", help="Use raw tmux instead of par")
    run_parser.add_argument("--no-watch", action="store_true", help="Don't watch after starting (default: watch is enabled)")
    run_parser.add_argument("--interval", type=int, default=60, help="Watch refresh interval in seconds")
    run_parser.add_argument("--auto-process", action="store_true", default=True, help="Auto commit/merge/kill on completion (default: True)")
    run_parser.add_argument("--no-auto-process", action="store_true", help="Disable auto processing")
    run_parser.add_argument("--no-auto-enter", action="store_true", help="Disable auto Enter key on confirmation prompts")
    run_parser.add_argument("--auto-continue", action="store_true", help="Auto-start next pending task when a slot opens up")
    run_parser.add_argument("--log-interval", type=int, default=30, help="Background monitor interval in seconds (default: 30)")
    run_parser.add_argument("--kiro-dir", default=".kiro", help="Path to .kiro directory")
    
    # Status command
    status_parser = subparsers.add_parser("status", help="Check execution status")
    status_parser.add_argument("spec_name", help="Name of the spec")
    status_parser.add_argument("--kiro-dir", default=".kiro", help="Path to .kiro directory")
    
    # Clean command
    clean_parser = subparsers.add_parser("clean", help="Clean up sessions and worktrees")
    clean_parser.add_argument("spec_name", help="Name of the spec")
    clean_parser.add_argument("--kiro-dir", default=".kiro", help="Path to .kiro directory")
    
    # Watch command (display-only, no actions)
    watch_parser = subparsers.add_parser("watch", help="Watch task status (display only)")
    watch_parser.add_argument("spec_name", help="Name of the spec")
    watch_parser.add_argument("--wave", type=int, help="Wave number to watch (default: 0)")
    watch_parser.add_argument("--interval", type=int, default=10, help="Display refresh interval in seconds")
    watch_parser.add_argument("--no-par", action="store_true", help="Use raw tmux instead of par")
    watch_parser.add_argument("--auto-continue", action="store_true", help="Watch all tasks globally instead of single wave")
    watch_parser.add_argument("--kiro-dir", default=".kiro", help="Path to .kiro directory")

    # Monitor command (starts BGR for existing tasks - automation without starting new tasks)
    monitor_parser = subparsers.add_parser("monitor", help="Start background monitor for existing running tasks")
    monitor_parser.add_argument("spec_name", help="Name of the spec")
    monitor_parser.add_argument("--interval", type=int, default=60, help="Log refresh interval in seconds")
    monitor_parser.add_argument("--no-par", action="store_true", help="Use raw tmux instead of par")
    monitor_parser.add_argument("--no-auto-process", action="store_true", help="Disable auto commit/merge/kill")
    monitor_parser.add_argument("--no-auto-enter", action="store_true", help="Disable auto-enter for waiting prompts")
    monitor_parser.add_argument("--auto-continue", action="store_true", help="Auto-start next pending tasks when slots open")
    monitor_parser.add_argument("--max-parallel", type=int, default=4, help="Max concurrent tasks for auto-continue")
    monitor_parser.add_argument("--watch", action="store_true", help="Also show watch display")
    monitor_parser.add_argument("--watch-interval", type=int, default=10, help="Watch display refresh interval")
    monitor_parser.add_argument("--kiro-dir", default=".kiro", help="Path to .kiro directory")

    # Master command (internal - runs master process that spawns workers)
    master_parser = subparsers.add_parser("master", help="Run master process (internal use)")
    master_parser.add_argument("spec_name", help="Name of the spec")
    master_parser.add_argument("--max-parallel", type=int, default=4, help="Max concurrent workers")
    master_parser.add_argument("--interval", type=int, default=30, help="Worker monitor interval in seconds")
    master_parser.add_argument("--kiro-dir", default=".kiro", help="Path to .kiro directory")

    # Worker command (internal - runs single worker that handles one task at a time)
    worker_parser = subparsers.add_parser("worker", help="Run worker process (internal use)")
    worker_parser.add_argument("spec_name", help="Name of the spec")
    worker_parser.add_argument("--worker-id", type=int, required=True, help="Worker ID")
    worker_parser.add_argument("--interval", type=int, default=30, help="Monitor interval in seconds")
    worker_parser.add_argument("--kiro-dir", default=".kiro", help="Path to .kiro directory")

    # Stop command - stop specific task and its worker
    stop_parser = subparsers.add_parser("stop", help="Stop a specific task and its worker")
    stop_parser.add_argument("spec_name", help="Name of the spec")
    stop_parser.add_argument("task_id", help="Task ID to stop (e.g., 5.2)")

    # Recover command - recover orphaned sessions from crashed workers
    recover_parser = subparsers.add_parser("recover", help="Recover orphaned sessions from crashed workers")
    recover_parser.add_argument("spec_name", help="Name of the spec")
    recover_parser.add_argument("--kiro-dir", default=".kiro", help="Path to .kiro directory")

    # Stop-all command - stop master and all workers
    stop_all_parser = subparsers.add_parser("stop-all", help="Stop master and all workers")
    stop_all_parser.add_argument("spec_name", help="Name of the spec")

    # Status command for master/workers
    master_status_parser = subparsers.add_parser("master-status", help="Check master and workers status")
    master_status_parser.add_argument("spec_name", help="Name of the spec")
    
    args = parser.parse_args()
    
    if not args.command:
        parser.print_help()
        return
    
    try:
        kiro_dir = getattr(args, 'kiro_dir', '.kiro')
        parser_obj = TasksParser(kiro_dir)
        tasks = parser_obj.parse(args.spec_name)

        state_executor = ParallelExecutor(spec_name=args.spec_name)
        saved_state = state_executor.load_state()
        apply_saved_state(tasks, saved_state, args.spec_name)
        
        if not tasks:
            print(f"No tasks found in spec: {args.spec_name}")
            return
        
        analyzer = DependencyAnalyzer()
        plan = analyzer.analyze(tasks)
        plan.spec_name = args.spec_name
        
        if args.command == "analyze":
            print_analysis(plan, tasks)
            if args.mermaid:
                print("\n" + "-"*60)
                print("MERMAID DIAGRAM")
                print("-"*60)
                print(generate_mermaid(plan))
        
        elif args.command == "run":
            should_watch = not getattr(args, 'no_watch', False)

            # Always show analysis for dry-run or no-watch mode
            if args.dry_run or not should_watch:
                print_analysis(plan, tasks)

            if not plan.waves:
                print("\n✅ All tasks completed! Nothing to run.")
                return

            if args.dry_run:
                print("\n[DRY RUN] Would execute the above plan")
                return

            # Get settings
            log_interval = getattr(args, 'log_interval', 30)

            print("\n" + "="*60)
            print("STARTING PARALLEL EXECUTION (Master-Worker Architecture)")
            print("="*60)
            print(f"\nMax parallel workers: {args.max_parallel}")
            print(f"Worker monitor interval: {log_interval}s")
            print(f"Watch mode: {'enabled' if should_watch else 'disabled'}")

            # Recover any orphaned sessions first
            executor = ParallelExecutor(spec_name=args.spec_name, use_par=not args.no_par)
            recovered = recover_orphaned_sessions(args.spec_name, executor)
            if recovered:
                print(f"\n🔄 Recovered {len(recovered)} orphaned session(s)")

            # Check if master already running
            is_running, existing_pid = is_master_running(args.spec_name)
            if is_running:
                print(f"\n📝 Master already running (PID: {existing_pid})")
                print("   Use 'stop-all' to stop existing execution first.")
            else:
                # Start master process which will spawn workers
                master_pid = start_master(
                    spec_name=args.spec_name,
                    max_parallel=args.max_parallel,
                    interval=log_interval,
                    project_root=Path(".")
                )
                print(f"\n📝 Master started (PID: {master_pid})")
                print(f"   Spawning up to {args.max_parallel} workers...")

            print(f"   Log files in: .kiro/specs/{args.spec_name}/logs/")

            if should_watch:
                # Give workers time to start
                time.sleep(2)
                executor = ParallelExecutor(spec_name=args.spec_name, use_par=not args.no_par)
                watch_wave(executor, plan, 0, args.interval, auto_continue=True)
            else:
                print(f"\n" + "-"*60)
                print("USEFUL COMMANDS:")
                print("-"*60)
                print(f"\n1. Check master/workers status:")
                print(f"   python3 tools/kiro-parallel/kiro_parallel.py master-status {args.spec_name}")

                print(f"\n2. Watch task progress:")
                print(f"   python3 tools/kiro-parallel/kiro_parallel.py watch {args.spec_name} --auto-continue")

                print(f"\n3. View logs:")
                print(f"   tail -f .kiro/specs/{args.spec_name}/logs/master.log")
                print(f"   tail -f .kiro/specs/{args.spec_name}/logs/worker-0.log")

                print(f"\n4. Stop a specific task:")
                print(f"   python3 tools/kiro-parallel/kiro_parallel.py stop {args.spec_name} 5.2")

                print(f"\n5. Stop all execution:")
                print(f"   python3 tools/kiro-parallel/kiro_parallel.py stop-all {args.spec_name}")

                print(f"\n6. Clean up when all done:")
                print(f"   python3 tools/kiro-parallel/kiro_parallel.py clean {args.spec_name}")

                print(f"\n✅ Execution started. Master and workers running independently.")
        
        elif args.command == "status":
            print_analysis(plan, tasks)
            
            executor = ParallelExecutor(spec_name=args.spec_name)
            state = executor.load_state()
            
            if state:
                print("\n" + "-"*60)
                print("SAVED STATE")
                print("-"*60)
                for task_id, task_state in state.get("tasks", {}).items():
                    status = task_state.get('status', 'unknown')
                    if task_state.get("session_name"):
                        print(f"  Task {task_id}: {status} (session: {task_state['session_name']})")
        
        elif args.command == "clean":
            # First stop master and all workers
            print("Stopping master and workers...")
            stop_master(args.spec_name)

            # Clean up completed tasks file
            completed_file = get_completed_tasks_file(args.spec_name)
            if completed_file.exists():
                completed_file.unlink()

            executor = ParallelExecutor(spec_name=args.spec_name)
            executor.cleanup(plan.tasks)
        
        elif args.command == "watch":
            if not plan.waves:
                print("\n✅ All tasks completed! Nothing to watch.")
                return

            executor = ParallelExecutor(
                spec_name=args.spec_name,
                use_par=not args.no_par
            )

            # Recover any orphaned sessions (crashed workers)
            recovered = recover_orphaned_sessions(args.spec_name, executor)
            if recovered:
                # Reload state after recovery
                saved_state = executor.load_state()
                apply_saved_state(plan.tasks, saved_state, args.spec_name)

            # Refresh session status to get accurate running count
            executor.refresh_session_status(plan.tasks)

            # Check if there are any running tasks
            running_count = sum(1 for t in plan.tasks.values() if t.status == TaskStatus.RUNNING)
            pending_count = sum(1 for t in plan.tasks.values() if t.status == TaskStatus.PENDING and not t.is_parent_task)

            if running_count == 0:
                if pending_count == 0:
                    print("\n✅ All tasks completed! Nothing to watch.")
                else:
                    print(f"\n⏳ No active sessions. {pending_count} task(s) pending.")
                    print("   Use 'run' command to start tasks, or 'monitor --auto-continue' to auto-start.")
                return

            auto_continue = getattr(args, 'auto_continue', False)
            wave_to_watch = args.wave if args.wave is not None else 0
            if 0 <= wave_to_watch < len(plan.waves):
                # watch is display-only, no BGR needed
                watch_wave(executor, plan, wave_to_watch, args.interval, auto_continue=auto_continue)
            else:
                print(f"Invalid wave number: {args.wave}")

        elif args.command == "monitor":
            # Start BGR for existing running tasks without starting new ones
            executor = ParallelExecutor(
                spec_name=args.spec_name,
                use_par=not args.no_par,
                max_parallel=args.max_parallel
            )
            executor.refresh_session_status(plan.tasks)

            # Count running tasks
            running_count = sum(1 for t in plan.tasks.values() if t.status == TaskStatus.RUNNING)
            pending_count = sum(1 for t in plan.tasks.values() if t.status == TaskStatus.PENDING and not t.is_parent_task)

            if running_count == 0 and pending_count == 0:
                print("\n✅ No running or pending tasks. Nothing to monitor.")
                return

            print(f"\n📊 Current state: {running_count} running, {pending_count} pending")

            auto_process = not getattr(args, 'no_auto_process', False)
            auto_enter = not getattr(args, 'no_auto_enter', False)
            auto_continue = getattr(args, 'auto_continue', False)
            log_interval = args.interval

            print(f"Auto-process: {auto_process}")
            print(f"Auto-enter: {auto_enter}")
            print(f"Auto-continue: {auto_continue}")
            print(f"Max parallel: {args.max_parallel}")

            # Start BGR
            start_log_refresher(executor, plan.tasks, plan, log_interval, auto_process, auto_enter, auto_continue)

            if args.watch:
                # Also show watch display
                wave_to_watch = 0
                watch_wave(executor, plan, wave_to_watch, args.watch_interval, auto_continue=auto_continue)
                stop_log_refresher()
            else:
                # Just keep BGR running until Ctrl+C
                print(f"\nBackground monitor running. Press Ctrl+C to stop.")
                print(f"Log files in: {executor.get_logs_dir()}\n")
                try:
                    while True:
                        time.sleep(1)
                except KeyboardInterrupt:
                    print("\n\nStopping monitor...")
                    stop_log_refresher()
                    print("Monitor stopped.")

        elif args.command == "master":
            # Internal command - run master process that spawns and monitors workers
            print(f"[Master] Starting for {args.spec_name}")
            print(f"[Master] Max parallel: {args.max_parallel}, Interval: {args.interval}s")

            project_root = Path(".")
            worker_pids: dict[int, int] = {}  # worker_id -> pid

            # Spawn initial workers
            for worker_id in range(args.max_parallel):
                pid = start_worker_bgr(
                    spec_name=args.spec_name,
                    worker_id=worker_id,
                    interval=args.interval,
                    project_root=project_root
                )
                if pid:
                    worker_pids[worker_id] = pid
                    print(f"[Master] Spawned worker {worker_id} (PID: {pid})")

            if not worker_pids:
                print(f"[Master] Failed to spawn any workers. Exiting.")
                return

            # Wait for all workers to exit
            print(f"[Master] Monitoring {len(worker_pids)} workers...")
            while worker_pids:
                for worker_id in list(worker_pids.keys()):
                    pid = worker_pids[worker_id]
                    if not is_process_running(pid):
                        print(f"[Master] Worker {worker_id} (PID: {pid}) exited")
                        del worker_pids[worker_id]
                        # Clean up PID/task files
                        pid_file = get_worker_pid_file(args.spec_name, worker_id, project_root)
                        task_file = get_worker_task_file(args.spec_name, worker_id, project_root)
                        if pid_file.exists():
                            pid_file.unlink()
                        if task_file.exists():
                            task_file.unlink()

                if worker_pids:
                    time.sleep(2)

            print(f"[Master] All workers exited. Master exiting.")
            # Clean up master PID file
            master_pid_file = get_master_pid_file(args.spec_name, project_root)
            if master_pid_file.exists():
                master_pid_file.unlink()

        elif args.command == "worker":
            # Internal command - run single worker that handles one task at a time
            worker_id = args.worker_id
            project_root = Path(".")

            print(f"[Worker {worker_id}] Starting for {args.spec_name}")
            print(f"[Worker {worker_id}] Interval: {args.interval}s")

            executor = ParallelExecutor(spec_name=args.spec_name)

            # Main worker loop
            while True:
                # First try to claim an already-running task (resume after restart)
                task = claim_running_task(args.spec_name, worker_id, executor, project_root)
                resumed = False

                if task:
                    print(f"[Worker {worker_id}] Resuming running task {task.id}: {task.title[:50]}")
                    task_start_time = task.start_time if task.start_time else time.time()
                    resumed = True
                else:
                    # Try to claim next pending task
                    task = claim_next_task(args.spec_name, worker_id, executor, project_root)

                    if not task:
                        print(f"[Worker {worker_id}] No more tasks available. Exiting.")
                        break

                    print(f"[Worker {worker_id}] Claimed task {task.id}: {task.title[:50]}")

                    # Start the task
                    executor.start_task(task)
                    task_start_time = time.time()

                    # Re-parse to get plan for saving state (only for new tasks)
                    fresh_tasks = parser_obj.parse(args.spec_name)
                    saved_state = executor.load_state()
                    apply_saved_state(fresh_tasks, saved_state, args.spec_name)
                    analyzer = DependencyAnalyzer()
                    plan = analyzer.analyze(fresh_tasks)
                    plan.spec_name = args.spec_name
                    executor.save_state(plan)

                print(f"[Worker {worker_id}] Task {task.id} {'resumed' if resumed else 'started'}, monitoring...")

                # Monitor this task until completion
                while True:
                    try:
                        # Refresh task state
                        executor.refresh_session_status({task.id: task})
                        executor.capture_session_output(task, silent=True, create_timestamped=False)

                        # Send Enter if waiting
                        if executor.send_enter_if_waiting(task):
                            print(f"[Worker {worker_id}] Sent Enter to task {task.id}")

                        # Check if completed
                        if executor.is_task_completed(task):
                            elapsed = int(time.time() - task_start_time)
                            print(f"[Worker {worker_id}] Task {task.id} completed after {elapsed}s, processing...")

                            # Process completed task (commit, merge, kill session)
                            executor.process_completed_task(task)

                            # Log for watch display
                            log_completed_task(args.spec_name, task, elapsed, project_root)

                            # Update state
                            fresh_tasks = parser_obj.parse(args.spec_name)
                            saved_state = executor.load_state()
                            apply_saved_state(fresh_tasks, saved_state, args.spec_name)
                            plan = analyzer.analyze(fresh_tasks)
                            plan.spec_name = args.spec_name
                            executor.save_state(plan)

                            print(f"[Worker {worker_id}] Task {task.id} processed successfully")
                            break

                        # Check if session still exists (might have been killed externally)
                        if task.session_name:
                            result = subprocess.run(
                                ["tmux", "has-session", "-t", task.session_name],
                                capture_output=True, check=False
                            )
                            if result.returncode != 0:
                                print(f"[Worker {worker_id}] Task {task.id} session no longer exists")
                                break

                        time.sleep(args.interval)

                    except Exception as e:
                        print(f"[Worker {worker_id}] Error monitoring task {task.id}: {e}")
                        time.sleep(args.interval)

                # Release task claim before trying to get next one
                release_task(args.spec_name, worker_id, project_root)

            # Cleanup worker files
            print(f"[Worker {worker_id}] Exiting.")
            pid_file = get_worker_pid_file(args.spec_name, worker_id, project_root)
            task_file = get_worker_task_file(args.spec_name, worker_id, project_root)
            if pid_file.exists():
                pid_file.unlink()
            if task_file.exists():
                task_file.unlink()

        elif args.command == "stop":
            # Stop a specific task and its worker
            task_id = args.task_id
            if stop_task_by_id(args.spec_name, task_id):
                print(f"✅ Stopped task {task_id} and its worker")
            else:
                print(f"⚠️ Task {task_id} not found or not running")

        elif args.command == "recover":
            # Recover orphaned sessions from crashed workers
            executor = ParallelExecutor(spec_name=args.spec_name)
            recovered = recover_orphaned_sessions(args.spec_name, executor)
            if recovered:
                print(f"✅ Recovered {len(recovered)} orphaned session(s):")
                for task_id, session_name in recovered:
                    print(f"   - Task {task_id}: {session_name}")
            else:
                print("No orphaned sessions found.")

        elif args.command == "stop-all":
            # Stop master and all workers
            if stop_master(args.spec_name):
                print(f"✅ Stopped master and all workers for {args.spec_name}")
            else:
                print(f"⚠️ No master running for {args.spec_name}")

            # Also kill any remaining tmux sessions
            executor = ParallelExecutor(spec_name=args.spec_name)
            for task in plan.tasks.values():
                if task.session_name:
                    subprocess.run(
                        ["tmux", "kill-session", "-t", task.session_name],
                        capture_output=True, check=False
                    )

        elif args.command == "master-status":
            # Check master and workers status
            is_running, master_pid = is_master_running(args.spec_name)
            if is_running:
                print(f"✅ Master running (PID: {master_pid})")
            else:
                print(f"⚠️ Master not running")

            # Show active workers
            workers = get_active_workers(args.spec_name)
            if workers:
                print(f"\nActive workers: {len(workers)}")
                print("-" * 60)
                for worker_id, pid, task_id in workers:
                    if task_id:
                        task = tasks.get(task_id)
                        title = task.title[:40] if task else "Unknown"
                        print(f"  Worker {worker_id} (PID: {pid}): Task {task_id} - {title}")
                    else:
                        print(f"  Worker {worker_id} (PID: {pid}): Claiming next task...")
            else:
                print("\nNo active workers")

            # Show recent completed tasks
            completed_file = get_completed_tasks_file(args.spec_name)
            if completed_file.exists():
                try:
                    entries = json.loads(completed_file.read_text())
                    if entries:
                        print(f"\nRecently completed tasks:")
                        print("-" * 60)
                        for entry in entries[-5:]:
                            mins, secs = divmod(entry['duration'], 60)
                            print(f"  ✅ {entry['task_id']}: {entry['title'][:40]} ({mins:02d}:{secs:02d})")
                except (json.JSONDecodeError, OSError):
                    pass

    except FileNotFoundError as e:
        print(f"Error: {e}")
    except ValueError as e:
        print(f"Error: {e}")


if __name__ == "__main__":
    main()
