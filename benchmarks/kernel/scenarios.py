#!/usr/bin/env python3
# Copyright (C) 2026 David Lin <dtwlin@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
"""Scenario definitions and command line builders for the 3-way kernel benchmark.

Defines the scenario matrix across categories:
  T: Whole-tree scans
  B: Single huge file ($BIG, big-24M.h, stdin)
  M: Multi-pattern sets
  L: Per-invocation latency
  X: xargs idiom
  U: UX out-of-the-box defaults vs naive invocations
  P: Pathological regexes

Builds exact command invocations for contestants:
  G0: Stock upstream GNU grep
  G1: Multi-threaded GNU grep fork
  R0: Stock ripgrep
  G2: Distro GNU grep
  X1: git grep
"""

import os
from typing import Any, Dict, List, Optional


class Scenario:
    def __init__(
        self,
        sid: str,
        category: str,
        description: str,
        is_core: bool = False,
        is_sweep: bool = False,
        equiv_mode: str = "sorted_lines",  # "sorted_lines", "sum_counts", "file_list", "exit_only", "none"
        expected_exit: int = 0,
        timeout_sec: int = 600,
        na_tools: Optional[List[str]] = None,
        notes: str = "",
    ):
        self.sid = sid
        self.category = category
        self.description = description
        self.is_core = is_core
        self.is_sweep = is_sweep
        self.equiv_mode = equiv_mode
        self.expected_exit = expected_exit
        self.timeout_sec = timeout_sec
        self.na_tools = set(na_tools or [])
        self.notes = notes

    def build_cmd(
        self,
        tool: str,
        bin_path: str,
        ksrc: str,
        kbench_dir: str,
        patterns_dir: str,
        threads: Optional[int] = None,
    ) -> Dict[str, Any]:
        """Builds process execution dictionary:

        {
            'argv': [...],
            'cwd': str,
            'env': dict,
            'stdin_file': str (optional),
            'pipe_cmd': list (optional),
        }
        """
        raise NotImplementedError


# Base scenario builder with common flag generation logic
class StandardScenario(Scenario):
    def __init__(
        self,
        sid: str,
        category: str,
        description: str,
        is_core: bool = False,
        is_sweep: bool = False,
        equiv_mode: str = "sorted_lines",
        expected_exit: int = 0,
        timeout_sec: int = 600,
        na_tools: Optional[List[str]] = None,
        notes: str = "",
        grep_args_fn=None,
        rg_args_fn=None,
        git_args_fn=None,
        stdin_fn=None,
        pipe_fn=None,
    ):
        super().__init__(
            sid=sid,
            category=category,
            description=description,
            is_core=is_core,
            is_sweep=is_sweep,
            equiv_mode=equiv_mode,
            expected_exit=expected_exit,
            timeout_sec=timeout_sec,
            na_tools=na_tools,
            notes=notes,
        )
        self.grep_args_fn = grep_args_fn
        self.rg_args_fn = rg_args_fn
        self.git_args_fn = git_args_fn
        self.stdin_fn = stdin_fn
        self.pipe_fn = pipe_fn

    def build_cmd(
        self,
        tool: str,
        bin_path: str,
        ksrc: str,
        kbench_dir: str,
        patterns_dir: str,
        threads: Optional[int] = None,
    ) -> Dict[str, Any]:
        if tool in self.na_tools:
            return {"na": True}

        big = os.path.join(kbench_dir, "linux-all.c")
        ctx = {
            "K": ksrc,
            "BIG": big,
            "KBENCH": kbench_dir,
            "P": patterns_dir,
        }

        env = {}
        cwd = None
        stdin_file = self.stdin_fn(ctx) if self.stdin_fn else None
        pipe_cmd = self.pipe_fn(ctx) if self.pipe_fn else None

        if tool in ("G0", "G1", "G2"):
            if tool == "G1" and threads is not None:
                env["GREP_THREADS"] = str(threads)
            argv = [bin_path] + self.grep_args_fn(ctx)

        elif tool == "R0":
            extra = []
            if threads is not None:
                extra = ["-j", str(threads)]
            argv = [bin_path] + extra + self.rg_args_fn(ctx)

        elif tool == "X1":
            extra = []
            if threads is not None:
                extra = ["--threads", str(threads)]
            git_info = self.git_args_fn(ctx)
            # git_info is either a list of args or dict with cwd & args
            if isinstance(git_info, dict):
                cwd = git_info.get("cwd", ksrc)
                git_args = git_info["args"]
            else:
                cwd = ksrc
                git_args = git_info
            argv = [bin_path, "grep"] + extra + git_args
        else:
            raise ValueError(f"Unknown tool: {tool}")

        return {
            "argv": argv,
            "cwd": cwd,
            "env": env,
            "stdin_file": stdin_file,
            "pipe_cmd": pipe_cmd,
        }


class XargsScenario(Scenario):
    """Scenario that feeds file paths via xargs -0 -a $KBENCH/files.z with cwd=$K."""

    def __init__(
        self,
        sid: str,
        category: str,
        description: str,
        is_core: bool = False,
        is_sweep: bool = False,
        equiv_mode: str = "sorted_lines",
        expected_exit: int = 123,
        timeout_sec: int = 600,
        na_tools: Optional[List[str]] = None,
        notes: str = "",
        grep_args_fn=None,
        rg_args_fn=None,
    ):
        super().__init__(
            sid=sid,
            category=category,
            description=description,
            is_core=is_core,
            is_sweep=is_sweep,
            equiv_mode=equiv_mode,
            expected_exit=expected_exit,
            timeout_sec=timeout_sec,
            na_tools=na_tools,
            notes=notes,
        )
        self.grep_args_fn = grep_args_fn
        self.rg_args_fn = rg_args_fn

    def build_cmd(
        self,
        tool: str,
        bin_path: str,
        ksrc: str,
        kbench_dir: str,
        patterns_dir: str,
        threads: Optional[int] = None,
    ) -> Dict[str, Any]:
        if tool in self.na_tools:
            return {"na": True}

        ctx = {
            "K": ksrc,
            "KBENCH": kbench_dir,
            "P": patterns_dir,
        }
        env = {}
        files_z = os.path.join(kbench_dir, "files.z")

        if tool in ("G0", "G1", "G2"):
            if tool == "G1" and threads is not None:
                env["GREP_THREADS"] = str(threads)
            tool_args = self.grep_args_fn(ctx)
        elif tool == "R0":
            extra = []
            if threads is not None:
                extra = ["-j", str(threads)]
            tool_args = extra + self.rg_args_fn(ctx)
        elif tool == "X1":
            extra = []
            if threads is not None:
                extra = ["--threads", str(threads)]
            tool_args = ["grep"] + extra + self.grep_args_fn(ctx)
        else:
            raise ValueError(f"Unknown tool: {tool}")

        argv = ["xargs", "-0", "-a", files_z, bin_path] + tool_args + ["--"]
        return {
            "argv": argv,
            "cwd": ksrc,
            "env": env,
            "stdin_file": None,
            "pipe_cmd": None,
        }


def make_scenarios() -> Dict[str, Scenario]:
    scenarios: Dict[str, Scenario] = {}

    def add(s: Scenario):
        scenarios[s.sid] = s

    # -------------------------------------------------------------
    # Category T: Whole-tree scans
    # -------------------------------------------------------------
    add(
        StandardScenario(
            sid="T-01",
            category="T",
            description="zero-match literal",
            is_core=True,
            is_sweep=True,
            equiv_mode="exit_only",
            expected_exit=1,
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-F", "ZZ_NO_SUCH_SYMBOL_0xDEADBEEF", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-F", "ZZ_NO_SUCH_SYMBOL_0xDEADBEEF", c["K"]],
            git_args_fn=lambda c: ["-F", "ZZ_NO_SUCH_SYMBOL_0xDEADBEEF"],
        )
    )

    add(
        StandardScenario(
            sid="T-02",
            category="T",
            description="rare literal",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-F", "kmalloc_array_node", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-F", "kmalloc_array_node", c["K"]],
            git_args_fn=lambda c: ["-F", "kmalloc_array_node"],
        )
    )

    add(
        StandardScenario(
            sid="T-03",
            category="T",
            description="moderate literal, line numbers",
            is_core=True,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-n", "-F", "spin_lock_irqsave", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-n", "-F", "spin_lock_irqsave", c["K"]],
            git_args_fn=lambda c: ["-n", "-F", "spin_lock_irqsave"],
        )
    )

    add(
        StandardScenario(
            sid="T-04",
            category="T",
            description="dense literal, line numbers",
            is_core=True,
            is_sweep=True,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-n", "-F", "static int", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-n", "-F", "static int", c["K"]],
            git_args_fn=lambda c: ["-n", "-F", "static int"],
        )
    )

    add(
        StandardScenario(
            sid="T-05",
            category="T",
            description="very dense literal",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-F", "return", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-N", "-F", "return", c["K"]],
            git_args_fn=lambda c: ["-F", "return"],
        )
    )

    add(
        StandardScenario(
            sid="T-06",
            category="T",
            description="files-with-matches",
            is_core=True,
            is_sweep=True,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-l", "-F", "EXPORT_SYMBOL_GPL", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-l", "-F", "EXPORT_SYMBOL_GPL", c["K"]],
            git_args_fn=lambda c: ["-l", "-F", "EXPORT_SYMBOL_GPL"],
        )
    )

    add(
        StandardScenario(
            sid="T-07",
            category="T",
            description="files-without-match",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-L", "-F", "SPDX-License-Identifier", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "--files-without-match", "-F", "SPDX-License-Identifier", c["K"]],
            git_args_fn=lambda c: ["-L", "-F", "SPDX-License-Identifier"],
        )
    )

    add(
        StandardScenario(
            sid="T-08",
            category="T",
            description="count per file",
            is_core=False,
            is_sweep=False,
            equiv_mode="sum_counts",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-c", "-F", "#include <linux/", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-c", "--include-zero", "-F", "#include <linux/", c["K"]],
            git_args_fn=lambda c: ["-c", "-F", "#include <linux/"],
        )
    )

    add(
        StandardScenario(
            sid="T-09",
            category="T",
            description="case-insensitive literal",
            is_core=True,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-i", "-F", "copyright", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-i", "-F", "copyright", c["K"]],
            git_args_fn=lambda c: ["-i", "-F", "copyright"],
        )
    )

    add(
        StandardScenario(
            sid="T-10",
            category="T",
            description="word match",
            is_core=True,
            is_sweep=True,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-w", "-n", "mutex_lock", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-w", "-n", "mutex_lock", c["K"]],
            git_args_fn=lambda c: ["-w", "-n", "mutex_lock"],
        )
    )

    add(
        StandardScenario(
            sid="T-11",
            category="T",
            description="DFA regex with must-literal",
            is_core=True,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-E", r"[a-z_]+_irq_handler\(", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", r"[a-z_]+_irq_handler\(", c["K"]],
            git_args_fn=lambda c: ["-E", r"[a-z_]+_irq_handler\("],
        )
    )

    add(
        StandardScenario(
            sid="T-12",
            category="T",
            description="anchored dense regex",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-E", r"^\s*#\s*include\s*<linux/[a-z_]+\.h>", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", r"^\s*#\s*include\s*<linux/[a-z_]+\.h>", c["K"]],
            git_args_fn=lambda c: ["-E", r"^\s*#\s*include\s*<linux/[a-z_]+\.h>"],
        )
    )

    add(
        StandardScenario(
            sid="T-13",
            category="T",
            description="literal alternation + word boundary",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-E", r"\b(kmalloc|kzalloc|vmalloc|kvmalloc)\(", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", r"\b(kmalloc|kzalloc|vmalloc|kvmalloc)\(", c["K"]],
            git_args_fn=lambda c: ["-E", r"\b(kmalloc|kzalloc|vmalloc|kvmalloc)\("],
        )
    )

    add(
        StandardScenario(
            sid="T-14",
            category="T",
            description="optional group + alternation",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-E", r"EXPORT_SYMBOL(_GPL)?\((dev|pci|usb)_[a-z_]+\)", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", r"EXPORT_SYMBOL(_GPL)?\((dev|pci|usb)_[a-z_]+\)", c["K"]],
            git_args_fn=lambda c: ["-E", r"EXPORT_SYMBOL(_GPL)?\((dev|pci|usb)_[a-z_]+\)"],
        )
    )

    add(
        StandardScenario(
            sid="T-15",
            category="T",
            description="literal-free regex",
            is_core=True,
            is_sweep=True,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-E", r"[0-9]{3}-[0-9]{3}-[0-9]{4}", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", r"[0-9]{3}-[0-9]{3}-[0-9]{4}", c["K"]],
            git_args_fn=lambda c: ["-E", r"[0-9]{3}-[0-9]{3}-[0-9]{4}"],
        )
    )

    add(
        StandardScenario(
            sid="T-16",
            category="T",
            description="PCRE2 lookaround, dense",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-P", r"(?<=\bstruct\s)\w+(?=\s*\*\s*\w+;)", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-P", r"(?<=\bstruct\s)\w+(?=\s*\*\s*\w+;)", c["K"]],
            git_args_fn=lambda c: ["-P", r"(?<=\bstruct\s)\w+(?=\s*\*\s*\w+;)"],
        )
    )

    add(
        StandardScenario(
            sid="T-17",
            category="T",
            description="PCRE2 rare with literal",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-P", r"EXPORT_SYMBOL_GPL\((?:dev|pci)_\w+_irq\w*\)", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-P", r"EXPORT_SYMBOL_GPL\((?:dev|pci)_\w+_irq\w*\)", c["K"]],
            git_args_fn=lambda c: ["-P", r"EXPORT_SYMBOL_GPL\((?:dev|pci)_\w+_irq\w*\)"],
        )
    )

    add(
        StandardScenario(
            sid="T-18",
            category="T",
            description="only-matching",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-o", "-E", r"EXPORT_SYMBOL(_GPL)?\([a-z_0-9]+\)", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-o", r"EXPORT_SYMBOL(_GPL)?\([a-z_0-9]+\)", c["K"]],
            git_args_fn=lambda c: ["-o", "-E", r"EXPORT_SYMBOL(_GPL)?\([a-z_0-9]+\)"],
        )
    )

    add(
        StandardScenario(
            sid="T-19",
            category="T",
            description="context, rare",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-n", "-C", "2", "-F", "kmalloc_array_node", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-n", "-C", "2", "-F", "kmalloc_array_node", c["K"]],
            git_args_fn=lambda c: ["-n", "-C", "2", "-F", "kmalloc_array_node"],
        )
    )

    add(
        StandardScenario(
            sid="T-20",
            category="T",
            description="context, moderate",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-n", "-C", "1", "-F", "spin_lock_irqsave", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-n", "-C", "1", "-F", "spin_lock_irqsave", c["K"]],
            git_args_fn=lambda c: ["-n", "-C", "1", "-F", "spin_lock_irqsave"],
        )
    )

    add(
        StandardScenario(
            sid="T-21",
            category="T",
            description="inverted count",
            is_core=False,
            is_sweep=False,
            equiv_mode="sum_counts",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-v", "-c", "-F", ";", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-v", "-c", "--include-zero", "-F", ";", c["K"]],
            git_args_fn=lambda c: ["-v", "-c", "-F", ";"],
        )
    )

    add(
        StandardScenario(
            sid="T-22",
            category="T",
            description="glob include",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "--include=*.c", "-n", "-F", "spin_lock_irqsave", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-g", "*.c", "-n", "-F", "spin_lock_irqsave", c["K"]],
            git_args_fn=lambda c: ["-n", "-F", "spin_lock_irqsave", "--", "*.c"],
        )
    )

    add(
        StandardScenario(
            sid="T-23",
            category="T",
            description="glob + sub-scope",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "--include=*.dts*", "-F", "interrupt-parent", os.path.join(c["K"], "arch/arm64/boot/dts")],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-g", "*.dts*", "-F", "interrupt-parent", os.path.join(c["K"], "arch/arm64/boot/dts")],
            git_args_fn=lambda c: ["-F", "interrupt-parent", "--", "arch/arm64/boot/dts/*.dts*"],
        )
    )

    add(
        StandardScenario(
            sid="T-24",
            category="T",
            description="max-count per file",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-m", "1", "-F", "EXPORT_SYMBOL_GPL", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-m", "1", "-F", "EXPORT_SYMBOL_GPL", c["K"]],
            git_args_fn=lambda c: ["-m", "1", "-F", "EXPORT_SYMBOL_GPL"],
        )
    )

    add(
        StandardScenario(
            sid="T-25a",
            category="T",
            description="quiet, no match",
            is_core=False,
            is_sweep=False,
            equiv_mode="exit_only",
            expected_exit=1,
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-q", "-F", "ZZ_NO_SUCH_SYMBOL_0xDEADBEEF", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-q", "-F", "ZZ_NO_SUCH_SYMBOL_0xDEADBEEF", c["K"]],
            git_args_fn=lambda c: ["-q", "-F", "ZZ_NO_SUCH_SYMBOL_0xDEADBEEF"],
        )
    )

    add(
        StandardScenario(
            sid="T-25b",
            category="T",
            description="quiet, immediate match",
            is_core=True,
            is_sweep=False,
            equiv_mode="exit_only",
            expected_exit=0,
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-q", "-F", "static", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-q", "-F", "static", c["K"]],
            git_args_fn=lambda c: ["-q", "-F", "static"],
        )
    )

    add(
        StandardScenario(
            sid="T-26",
            category="T",
            description="byte offsets",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            na_tools=["X1"],
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-b", "-n", "-F", "spin_lock_irqsave", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-b", "-n", "-F", "spin_lock_irqsave", c["K"]],
            git_args_fn=lambda c: [],
        )
    )

    add(
        StandardScenario(
            sid="T-27",
            category="T",
            description="sub-scope drivers/",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-n", "-F", "spin_lock_irqsave", os.path.join(c["K"], "drivers")],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-n", "-F", "spin_lock_irqsave", os.path.join(c["K"], "drivers")],
            git_args_fn=lambda c: ["-n", "-F", "spin_lock_irqsave", "--", "drivers"],
        )
    )

    add(
        StandardScenario(
            sid="T-28",
            category="T",
            description="sub-scope kernel mm fs",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-n", "-E", r"might_sleep\(\)", os.path.join(c["K"], "kernel"), os.path.join(c["K"], "mm"), os.path.join(c["K"], "fs")],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-n", r"might_sleep\(\)", os.path.join(c["K"], "kernel"), os.path.join(c["K"], "mm"), os.path.join(c["K"], "fs")],
            git_args_fn=lambda c: ["-n", "-E", r"might_sleep\(\)", "--", "kernel", "mm", "fs"],
        )
    )

    # -------------------------------------------------------------
    # Category B: Single huge file ($BIG, big-24M.h, stdin)
    # -------------------------------------------------------------
    add(
        StandardScenario(
            sid="B-01",
            category="B",
            description="rare literal ($BIG)",
            is_core=True,
            is_sweep=True,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["--color=never", "-F", "kmalloc_array_node", c["BIG"]],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-F", "kmalloc_array_node", c["BIG"]],
            git_args_fn=lambda c: {"cwd": c["KBENCH"], "args": ["--no-index", "-h", "-F", "kmalloc_array_node", "linux-all.c"]},
        )
    )

    add(
        StandardScenario(
            sid="B-02",
            category="B",
            description="count dense ($BIG)",
            is_core=False,
            is_sweep=False,
            equiv_mode="sum_counts",
            grep_args_fn=lambda c: ["--color=never", "-c", "-F", "static int", c["BIG"]],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-c", "--include-zero", "-F", "static int", c["BIG"]],
            git_args_fn=lambda c: {"cwd": c["KBENCH"], "args": ["--no-index", "-h", "-c", "-F", "static int", "linux-all.c"]},
        )
    )

    add(
        StandardScenario(
            sid="B-03",
            category="B",
            description="moderate, line numbers ($BIG)",
            is_core=True,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["--color=never", "-n", "-F", "spin_lock_irqsave", c["BIG"]],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-n", "-F", "spin_lock_irqsave", c["BIG"]],
            git_args_fn=lambda c: {"cwd": c["KBENCH"], "args": ["--no-index", "-h", "-n", "-F", "spin_lock_irqsave", "linux-all.c"]},
        )
    )

    add(
        StandardScenario(
            sid="B-04",
            category="B",
            description="DFA regex ($BIG)",
            is_core=True,
            is_sweep=True,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["--color=never", "-E", r"[a-z_]+_irq_handler\(", c["BIG"]],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", r"[a-z_]+_irq_handler\(", c["BIG"]],
            git_args_fn=lambda c: {"cwd": c["KBENCH"], "args": ["--no-index", "-h", "-E", r"[a-z_]+_irq_handler\(", "linux-all.c"]},
        )
    )

    add(
        StandardScenario(
            sid="B-05",
            category="B",
            description="case-insensitive dense ($BIG)",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["--color=never", "-n", "-i", "-F", "copyright", c["BIG"]],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-n", "-i", "-F", "copyright", c["BIG"]],
            git_args_fn=lambda c: {"cwd": c["KBENCH"], "args": ["--no-index", "-h", "-n", "-i", "-F", "copyright", "linux-all.c"]},
        )
    )

    add(
        StandardScenario(
            sid="B-06",
            category="B",
            description="word match ($BIG)",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["--color=never", "-w", "mutex_lock", c["BIG"]],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-w", "mutex_lock", c["BIG"]],
            git_args_fn=lambda c: {"cwd": c["KBENCH"], "args": ["--no-index", "-h", "-w", "mutex_lock", "linux-all.c"]},
        )
    )

    add(
        StandardScenario(
            sid="B-07",
            category="B",
            description="literal-free regex ($BIG)",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["--color=never", "-E", r"[0-9]{3}-[0-9]{3}-[0-9]{4}", c["BIG"]],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", r"[0-9]{3}-[0-9]{3}-[0-9]{4}", c["BIG"]],
            git_args_fn=lambda c: {"cwd": c["KBENCH"], "args": ["--no-index", "-h", "-E", r"[0-9]{3}-[0-9]{3}-[0-9]{4}", "linux-all.c"]},
        )
    )

    add(
        StandardScenario(
            sid="B-08",
            category="B",
            description="PCRE2 lookaround ($BIG)",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["--color=never", "-P", r"(?<=\bstruct\s)\w+(?=\s*\*\s*\w+;)", c["BIG"]],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-P", r"(?<=\bstruct\s)\w+(?=\s*\*\s*\w+;)", c["BIG"]],
            git_args_fn=lambda c: {"cwd": c["KBENCH"], "args": ["--no-index", "-h", "-P", r"(?<=\bstruct\s)\w+(?=\s*\*\s*\w+;)", "linux-all.c"]},
        )
    )

    add(
        StandardScenario(
            sid="B-09",
            category="B",
            description="context ($BIG)",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["--color=never", "-n", "-C", "2", "-F", "kmalloc_array_node", c["BIG"]],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-n", "-C", "2", "-F", "kmalloc_array_node", c["BIG"]],
            git_args_fn=lambda c: {"cwd": c["KBENCH"], "args": ["--no-index", "-h", "-n", "-C", "2", "-F", "kmalloc_array_node", "linux-all.c"]},
        )
    )

    add(
        StandardScenario(
            sid="B-10",
            category="B",
            description="only-matching ($BIG)",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["--color=never", "-o", "-E", r"EXPORT_SYMBOL(_GPL)?\([a-z_0-9]+\)", c["BIG"]],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-o", r"EXPORT_SYMBOL(_GPL)?\([a-z_0-9]+\)", c["BIG"]],
            git_args_fn=lambda c: {"cwd": c["KBENCH"], "args": ["--no-index", "-h", "-o", "-E", r"EXPORT_SYMBOL(_GPL)?\([a-z_0-9]+\)", "linux-all.c"]},
        )
    )

    add(
        StandardScenario(
            sid="B-11",
            category="B",
            description="inverted count ($BIG)",
            is_core=False,
            is_sweep=False,
            equiv_mode="sum_counts",
            grep_args_fn=lambda c: ["--color=never", "-v", "-c", "-F", ";", c["BIG"]],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-v", "-c", "--include-zero", "-F", ";", c["BIG"]],
            git_args_fn=lambda c: {"cwd": c["KBENCH"], "args": ["--no-index", "-h", "-v", "-c", "-F", ";", "linux-all.c"]},
        )
    )

    add(
        StandardScenario(
            sid="B-12",
            category="B",
            description="stdin redirect (< $BIG)",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            na_tools=["X1"],
            grep_args_fn=lambda c: ["--color=never", "-F", "kmalloc_array_node"],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-F", "kmalloc_array_node"],
            git_args_fn=lambda c: [],
            stdin_fn=lambda c: c["BIG"],
        )
    )

    add(
        StandardScenario(
            sid="B-13",
            category="B",
            description="FIFO pipe (cat $BIG |)",
            is_core=True,
            is_sweep=False,
            equiv_mode="sorted_lines",
            na_tools=["X1"],
            grep_args_fn=lambda c: ["--color=never", "-n", "-F", "spin_lock_irqsave"],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-n", "-F", "spin_lock_irqsave"],
            git_args_fn=lambda c: [],
            pipe_fn=lambda c: ["cat", c["BIG"]],
        )
    )

    add(
        StandardScenario(
            sid="B-14",
            category="B",
            description="real 24 MB header (big-24M.h)",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["--color=never", "-n", "-F", "_MASK", os.path.join(c["KBENCH"], "big-24M.h")],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-n", "-F", "_MASK", os.path.join(c["KBENCH"], "big-24M.h")],
            git_args_fn=lambda c: {"cwd": c["KBENCH"], "args": ["--no-index", "-h", "-n", "-F", "_MASK", "big-24M.h"]},
        )
    )

    # -------------------------------------------------------------
    # Category M: Multi-pattern sets
    # -------------------------------------------------------------
    # sym_100.txt: 2,347 lines across 907 files (0.005% of tree)
    add(
        StandardScenario(
            sid="M-01",
            category="M",
            description="100 literals, tree",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            na_tools=["X1"],
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-F", "-f", os.path.join(c["P"], "sym_100.txt"), c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-F", "-f", os.path.join(c["P"], "sym_100.txt"), c["K"]],
            git_args_fn=lambda c: ["-F", "-f", os.path.join(c["P"], "sym_100.txt")],
        )
    )

    # sym_1000.txt: 21,748 lines across 8,021 files (0.050% of tree)
    add(
        StandardScenario(
            sid="M-02",
            category="M",
            description="1,000 literals, tree",
            is_core=True,
            is_sweep=True,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-F", "-f", os.path.join(c["P"], "sym_1000.txt"), c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-F", "-f", os.path.join(c["P"], "sym_1000.txt"), c["K"]],
            git_args_fn=lambda c: ["-F", "-f", os.path.join(c["P"], "sym_1000.txt")],
        )
    )

    # sym_10000.txt: 231,638 lines across 27,501 files (0.529% of tree)
    add(
        StandardScenario(
            sid="M-03",
            category="M",
            description="10,000 literals, tree",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            na_tools=["X1"],
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-F", "-f", os.path.join(c["P"], "sym_10000.txt"), c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-F", "-f", os.path.join(c["P"], "sym_10000.txt"), c["K"]],
            git_args_fn=lambda c: ["-F", "-f", os.path.join(c["P"], "sym_10000.txt")],
        )
    )

    add(
        StandardScenario(
            sid="M-04",
            category="M",
            description="1,000 literals, -i",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            na_tools=["X1"],
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-i", "-F", "-f", os.path.join(c["P"], "sym_1000.txt"), c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-i", "-F", "-f", os.path.join(c["P"], "sym_1000.txt"), c["K"]],
            git_args_fn=lambda c: ["-i", "-F", "-f", os.path.join(c["P"], "sym_1000.txt")],
        )
    )

    add(
        StandardScenario(
            sid="M-05",
            category="M",
            description="1,000 literals, -w",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            na_tools=["X1"],
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-w", "-F", "-f", os.path.join(c["P"], "sym_1000.txt"), c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-w", "-F", "-f", os.path.join(c["P"], "sym_1000.txt"), c["K"]],
            git_args_fn=lambda c: ["-w", "-F", "-f", os.path.join(c["P"], "sym_1000.txt")],
        )
    )

    add(
        StandardScenario(
            sid="M-06",
            category="M",
            description="1,000 literals, $BIG",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["--color=never", "-F", "-f", os.path.join(c["P"], "sym_1000.txt"), c["BIG"]],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-F", "-f", os.path.join(c["P"], "sym_1000.txt"), c["BIG"]],
            git_args_fn=lambda c: {"cwd": c["KBENCH"], "args": ["--no-index", "-h", "-F", "-f", os.path.join(c["P"], "sym_1000.txt"), "linux-all.c"]},
        )
    )

    # -------------------------------------------------------------
    # Category L: Per-invocation latency
    # -------------------------------------------------------------
    add(
        StandardScenario(
            sid="L-00",
            category="L",
            description="harness floor (/bin/true)",
            is_core=True,
            is_sweep=False,
            equiv_mode="exit_only",
            expected_exit=0,
            grep_args_fn=lambda c: [],  # special-cased in runner to /bin/true
            rg_args_fn=lambda c: [],
            git_args_fn=lambda c: [],
        )
    )

    add(
        StandardScenario(
            sid="L-01",
            category="L",
            description="one small file (init/main.c)",
            is_core=True,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["--color=never", "-n", "start_kernel", os.path.join(c["K"], "init/main.c")],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-n", "start_kernel", os.path.join(c["K"], "init/main.c")],
            git_args_fn=lambda c: ["-h", "-n", "start_kernel", "--", "init/main.c"],
        )
    )

    add(
        StandardScenario(
            sid="L-02",
            category="L",
            description="tiny tree (kernel/sched)",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-n", "rq_lock", os.path.join(c["K"], "kernel/sched")],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-n", "rq_lock", os.path.join(c["K"], "kernel/sched")],
            git_args_fn=lambda c: ["-n", "rq_lock", "--", "kernel/sched"],
        )
    )

    add(
        StandardScenario(
            sid="L-03",
            category="L",
            description="quiet first match in big file",
            is_core=False,
            is_sweep=False,
            equiv_mode="exit_only",
            expected_exit=0,
            grep_args_fn=lambda c: ["--color=never", "-q", "-F", "static", c["BIG"]],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-q", "-F", "static", c["BIG"]],
            git_args_fn=lambda c: {"cwd": c["KBENCH"], "args": ["--no-index", "-h", "-q", "-F", "static", "linux-all.c"]},
        )
    )

    # -------------------------------------------------------------
    # Category X, U, P: Special / UX / Pathological
    # -------------------------------------------------------------
    add(
        XargsScenario(
            sid="X-01",
            category="X",
            description="git ls-files | xargs",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            expected_exit=123,
            na_tools=["X1"],
            grep_args_fn=lambda c: ["--color=never", "-n", "-F", "spin_lock_irqsave"],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-n", "-F", "spin_lock_irqsave"],
        )
    )

    add(
        StandardScenario(
            sid="U-01",
            category="U",
            description="out-of-the-box UX",
            is_core=True,
            is_sweep=False,
            equiv_mode="sorted_lines",
            na_tools=["X1"],
            notes="rg uses default gitignore-aware walker; grep uses -r --exclude-dir=.git",
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-n", "-F", "spin_lock_irqsave", c["K"]],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-n", "-F", "spin_lock_irqsave", c["K"]],
            git_args_fn=lambda c: [],
        )
    )

    add(
        StandardScenario(
            sid="U-02",
            category="U",
            description="naive invocation (scans 6 GB .git)",
            is_core=True,
            is_sweep=False,
            equiv_mode="none",
            na_tools=["X1"],
            notes="grep scans 6 GB .git without --exclude-dir; rg skips .git by default",
            grep_args_fn=lambda c: ["-r", "--color=never", "-n", "-F", "spin_lock_irqsave", c["K"]],
            rg_args_fn=lambda c: ["--color=never", "--no-heading", "-n", "-F", "spin_lock_irqsave", c["K"]],
            git_args_fn=lambda c: [],
        )
    )

    add(
        StandardScenario(
            sid="P-01",
            category="P",
            description="backreference (\\b([a-z]{4,})_\\1\\b)",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            timeout_sec=900,
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-E", r"\b([a-z]{4,})_\1\b", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-P", r"\b([a-z]{4,})_\1\b", c["K"]],
            git_args_fn=lambda c: ["-P", r"\b([a-z]{4,})_\1\b"],
        )
    )

    # sym_1000_regex.txt: 14,066 lines across 6,463 files (0.032% of tree)
    add(
        StandardScenario(
            sid="P-02",
            category="P",
            description="1,000-way regex alternation",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            timeout_sec=900,
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-E", "-f", os.path.join(c["P"], "sym_1000_regex.txt"), c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", "-f", os.path.join(c["P"], "sym_1000_regex.txt"), c["K"]],
            git_args_fn=lambda c: ["-E", "-f", os.path.join(c["P"], "sym_1000_regex.txt")],
        )
    )

    add(
        StandardScenario(
            sid="P-03",
            category="P",
            description="dot-star heavy (.*_lock.*_unlock)",
            is_core=False,
            is_sweep=False,
            equiv_mode="sorted_lines",
            timeout_sec=900,
            grep_args_fn=lambda c: ["-r", "--exclude-dir=.git", "--color=never", "-E", ".*_lock.*_unlock", c["K"]],
            rg_args_fn=lambda c: ["-uu", "-g", "!.git", "--color=never", "--no-heading", ".*_lock.*_unlock", c["K"]],
            git_args_fn=lambda c: ["-E", ".*_lock.*_unlock"],
        )
    )

    return scenarios
