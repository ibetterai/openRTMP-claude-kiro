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
        max_parallel: int = 4,
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

    def capture_session_output(self, task: Task, lines: int = 10000) -> str:
        """Capture tmux session output and save to file with token summary"""
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
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        log_file = logs_dir / f"task-{task.id.replace('.', '-')}_{timestamp}.log"
        log_file.write_text(output_with_summary)

        # Also maintain a "latest" symlink/file
        latest_file = logs_dir / f"task-{task.id.replace('.', '-')}_latest.log"
        latest_file.write_text(output_with_summary)

        # Print summary to console
        print(summary_text)

        return output

    def is_session_waiting_for_input(self, task: Task) -> bool:
        """Check if session is waiting for user input/confirmation"""
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

        # Check last few lines for prompt indicators
        last_lines = '\n'.join(lines[-10:]) if len(lines) >= 10 else output

        # Patterns indicating waiting for input
        waiting_patterns = [
            r'>\s*$',  # Empty prompt
            r'0 tokens\s*$',  # Claude showing 0 tokens (hasn't started)
            r'\(y/n\)',  # Yes/no prompt
            r'\[Y/n\]',  # Yes/no prompt
            r'\[y/N\]',  # Yes/no prompt
            r'Press Enter',  # Press enter prompt
            r'Continue\?',  # Continue prompt
        ]

        for pattern in waiting_patterns:
            if re.search(pattern, last_lines, re.IGNORECASE):
                # Check if there's actual activity (tokens > 0 means working)
                if '0 tokens' in last_lines and 'Thinking' not in last_lines:
                    return True
                # Check for prompt at end without activity
                if re.search(r'>\s*$', lines[-1] if lines else ''):
                    # Make sure it's not actively thinking
                    if 'Thinking' not in last_lines and 'tokens' not in last_lines:
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
        """Check if task has completed its work"""
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

        # Check for completion indicators
        completion_patterns = [
            r'Task.*completed',
            r'Implementation complete',
            r'All tests pass',
            r'✅.*complete',
            r'Remaining tasks.*:',  # Shows next steps = current done
        ]

        # Check if at idle prompt with work done
        lines = output.strip().split('\n')
        last_lines = '\n'.join(lines[-15:]) if len(lines) >= 15 else output

        for pattern in completion_patterns:
            if re.search(pattern, last_lines, re.IGNORECASE):
                # Verify it's at an idle prompt
                if lines and re.search(r'>\s*$', lines[-1]):
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
    
    def start_task(self, task: Task):
        """Start execution of a single task"""
        worktree_path = self.create_worktree(task)
        session_name = self.create_session(task, worktree_path)
        
        time.sleep(1)
        
        # Start Claude Code
        self.send_command(session_name, "/usr/local/bin/c")
        time.sleep(2)
        
        # Send the implementation command
        impl_cmd = f"/kiro:spec-impl {self.spec_name} {task.id}"
        self.send_command(session_name, impl_cmd)
        
        task.start_time = time.time()
        task.status = TaskStatus.RUNNING
        print(f"  ✓ Started Task {task.id}: {task.title[:50]}...")
        print(f"    Session: {session_name}")
        print(f"    Worktree: {worktree_path}")
    
    def _start_pending_tasks(self, tasks: dict[str, Task], wave_task_ids: list[str], announce: bool = True) -> int:
        """Start pending tasks respecting max_parallel and current running count"""
        pending_tasks = [
            tasks[tid] for tid in wave_task_ids
            if tasks[tid].status == TaskStatus.PENDING
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
    pending = len(all_tasks) - completed
    
    print(f"\nSpec: {plan.spec_name}")
    print(f"Total Tasks: {len(all_tasks)} ({completed} completed, {pending} pending)")
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
                print(f"   ⬜ Task {task.id}{parallel_marker}: {task.title[:45]}")
                if task.dependencies:
                    print(f"      └─ Depends on: {deps}")


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


def watch_wave(executor: ParallelExecutor, plan: ExecutionPlan, wave_idx: int, interval: int, start_pending: bool = True, auto_process: bool = True):
    """Watch a wave with auto-completion handling.

    Features:
    - Writes session output to logs/
    - Auto-sends Enter if waiting for confirmation
    - Auto-commits, merges, and kills session on completion
    """
    wave_task_ids = plan.waves[wave_idx]
    print(f"\nWatching Wave {wave_idx + 1} (interval: {interval}s). Press Ctrl+C to stop.")
    if auto_process:
        print("Auto-processing enabled: will commit/merge/kill on task completion.\n")
    else:
        print("Auto-processing disabled.\n")

    start_time = time.time()
    printed_lines = 0
    processed_tasks: set[str] = set()  # Track tasks we've already processed

    try:
        # Initial refresh before loop
        executor.refresh_session_status(plan.tasks)
        if start_pending:
            executor._start_pending_tasks(plan.tasks, wave_task_ids, announce=False)
            executor.save_state(plan)

        while True:
            executor.refresh_session_status(plan.tasks)

            # Check for tasks waiting for input and auto-send Enter
            for tid in wave_task_ids:
                task = plan.tasks[tid]
                if task.status == TaskStatus.RUNNING:
                    if executor.send_enter_if_waiting(task):
                        print(f"\n⏎ Sent Enter to task {task.id} (was waiting)")

            # Check for completed tasks and process them
            if auto_process:
                for tid in wave_task_ids:
                    task = plan.tasks[tid]
                    if tid not in processed_tasks:
                        if executor.is_task_completed(task):
                            # Clear the watch display before processing
                            if printed_lines:
                                print(f"\033[{printed_lines}F", end="")
                                for _ in range(printed_lines):
                                    print(f"\r\033[K")
                                printed_lines = 0

                            executor.process_completed_task(task)
                            processed_tasks.add(tid)
                            executor.save_state(plan)
                            print()  # Add newline after processing

            # Start more pending tasks if slots available
            if start_pending:
                started = executor._start_pending_tasks(plan.tasks, wave_task_ids, announce=False)
                if started > 0:
                    executor.save_state(plan)

            running_tasks = [
                plan.tasks[tid] for tid in wave_task_ids
                if plan.tasks[tid].status == TaskStatus.RUNNING
            ]
            pending_tasks = [
                plan.tasks[tid] for tid in wave_task_ids
                if plan.tasks[tid].status == TaskStatus.PENDING
            ]
            completed_tasks = [
                plan.tasks[tid] for tid in wave_task_ids
                if plan.tasks[tid].status == TaskStatus.COMPLETED
            ]

            elapsed_total = int(time.time() - start_time)
            mins, secs = divmod(elapsed_total, 60)
            hours, mins = divmod(mins, 60)
            elapsed_str = f"{hours:02d}:{mins:02d}:{secs:02d}"
            header = f"Wave {wave_idx + 1} | {elapsed_str} | ✅{len(completed_tasks)} 🔄{len(running_tasks)} ⏳{len(pending_tasks)}"

            lines: list[str] = [header]

            if not running_tasks:
                if not pending_tasks:
                    lines.append(f"All {len(completed_tasks)} tasks completed! Exiting watch.")
                    line_count = max(printed_lines, len(lines))
                    lines.extend([""] * (line_count - len(lines)))
                    if printed_lines:
                        print(f"\033[{printed_lines}F", end="")
                    for ln in lines:
                        print(f"\r{ln}\033[K")
                    print()
                    return
                if not start_pending:
                    lines.append("No running tasks. Exiting watch.")
                    line_count = max(printed_lines, len(lines))
                    lines.extend([""] * (line_count - len(lines)))
                    if printed_lines:
                        print(f"\033[{printed_lines}F", end="")
                    for ln in lines:
                        print(f"\r{ln}\033[K")
                    print()
                    return

            for task in running_tasks:
                elapsed = max(0, int(time.time() - task.start_time)) if task.start_time else 0
                bar_width = 20
                ratio = (elapsed % 60) / 60 if elapsed else 0
                filled = int(bar_width * ratio)
                bar = "[" + "#" * filled + "." * (bar_width - filled) + "]"
                mins_t, secs_t = divmod(elapsed, 60)
                hours_t, mins_t = divmod(mins_t, 60)
                elapsed_task = f"{hours_t:02d}:{mins_t:02d}:{secs_t:02d}"
                title = task.title[:40]
                session = task.session_name or "-"
                lines.append(f"{task.id:<6} {bar} {elapsed_task} | {title:<40} | {session}")

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
        print("\n\nWatch interrupted. Saving state...")
        # Capture output for all running tasks before exit
        for tid in wave_task_ids:
            task = plan.tasks[tid]
            if task.status == TaskStatus.RUNNING and task.session_name:
                executor.capture_session_output(task)
        executor.save_state(plan)
        print("State saved. Sessions still running in background.")


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
    run_parser.add_argument("--watch", action="store_true", help="Watch task status until tasks stop")
    run_parser.add_argument("--interval", type=int, default=60, help="Watch refresh interval in seconds")
    run_parser.add_argument("--auto-process", action="store_true", default=True, help="Auto commit/merge/kill on completion (default: True)")
    run_parser.add_argument("--no-auto-process", action="store_true", help="Disable auto processing")
    run_parser.add_argument("--kiro-dir", default=".kiro", help="Path to .kiro directory")
    
    # Status command
    status_parser = subparsers.add_parser("status", help="Check execution status")
    status_parser.add_argument("spec_name", help="Name of the spec")
    status_parser.add_argument("--kiro-dir", default=".kiro", help="Path to .kiro directory")
    
    # Clean command
    clean_parser = subparsers.add_parser("clean", help="Clean up sessions and worktrees")
    clean_parser.add_argument("spec_name", help="Name of the spec")
    clean_parser.add_argument("--kiro-dir", default=".kiro", help="Path to .kiro directory")
    
    # Watch command
    watch_parser = subparsers.add_parser("watch", help="Watch task status for a wave")
    watch_parser.add_argument("spec_name", help="Name of the spec")
    watch_parser.add_argument("--wave", type=int, help="Wave number to watch (default: 0)")
    watch_parser.add_argument("--max-parallel", type=int, default=4, help="Max concurrent tasks")
    watch_parser.add_argument("--interval", type=int, default=60, help="Watch refresh interval in seconds")
    watch_parser.add_argument("--no-par", action="store_true", help="Use raw tmux instead of par")
    watch_parser.add_argument("--auto-process", action="store_true", default=True, help="Auto commit/merge/kill on completion")
    watch_parser.add_argument("--no-auto-process", action="store_true", help="Disable auto processing")
    watch_parser.add_argument("--kiro-dir", default=".kiro", help="Path to .kiro directory")
    
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
            if not args.watch:
                print_analysis(plan, tasks)
            
            if not plan.waves:
                print("\n✅ All tasks completed! Nothing to run.")
                return
            
            if args.dry_run:
                print("\n[DRY RUN] Would execute the above plan")
                return
            
            executor = ParallelExecutor(
                spec_name=args.spec_name,
                max_parallel=args.max_parallel,
                use_par=not args.no_par
            )
            executor.refresh_session_status(plan.tasks)
            
            auto_process = not getattr(args, 'no_auto_process', False)

            if args.wave is not None:
                if 0 <= args.wave < len(plan.waves):
                    executor.execute_wave(args.wave, plan.tasks, plan.waves[args.wave])
                    executor.save_state(plan)
                    if args.watch:
                        watch_wave(executor, plan, args.wave, args.interval, start_pending=True, auto_process=auto_process)
                else:
                    print(f"Invalid wave number: {args.wave}")
            else:
                print("\n" + "="*60)
                print("STARTING PARALLEL EXECUTION")
                print("="*60)
                print(f"\nUsing: {'par' if executor.use_par else 'tmux'}")
                print(f"Max parallel: {args.max_parallel}")
                print(f"Auto-process: {auto_process}")

                # Execute first wave
                executor.execute_wave(0, plan.tasks, plan.waves[0])
                executor.save_state(plan)

                if args.watch:
                    watch_wave(executor, plan, 0, args.interval, start_pending=True, auto_process=auto_process)
                
                print(f"\n" + "="*60)
                print("NEXT STEPS")
                print("="*60)
                print(f"\n1. Monitor sessions:")
                if executor.use_par:
                    print(f"   par control-center")
                    print(f"   par ls")
                else:
                    print(f"   tmux ls")
                    print(f"   tmux attach -t <session-name>")
                
                print(f"\n2. When Wave 1 tasks complete, run:")
                print(f"   cd {executor.project_root}")
                print(f"   python3 tools/kiro-parallel/kiro_parallel.py run {args.spec_name} --wave 1")
                
                print(f"\n3. Clean up when all done:")
                print(f"   python3 tools/kiro-parallel/kiro_parallel.py clean {args.spec_name}")
        
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
            executor = ParallelExecutor(spec_name=args.spec_name)
            executor.cleanup(plan.tasks)
        
        elif args.command == "watch":
            if not plan.waves:
                print("\n✅ All tasks completed! Nothing to watch.")
                return

            executor = ParallelExecutor(
                spec_name=args.spec_name,
                max_parallel=args.max_parallel,
                use_par=not args.no_par
            )
            executor.refresh_session_status(plan.tasks)

            auto_process = not getattr(args, 'no_auto_process', False)
            wave_to_watch = args.wave if args.wave is not None else 0
            if 0 <= wave_to_watch < len(plan.waves):
                watch_wave(executor, plan, wave_to_watch, args.interval, start_pending=False, auto_process=auto_process)
            else:
                print(f"Invalid wave number: {args.wave}")
    
    except FileNotFoundError as e:
        print(f"Error: {e}")
    except ValueError as e:
        print(f"Error: {e}")


if __name__ == "__main__":
    main()
