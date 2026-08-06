#!/usr/bin/env python3
"""Evaluate the configured DeepSeek task classifier against a fixed corpus."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
from typing import Any

from audio_receiver import load_env_file
from cloud_services import DeepSeekClient, DeepSeekConfig, TaskRecord


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--cases",
        type=Path,
        default=Path("tools/task_input_cases.json"),
    )
    parser.add_argument(
        "--cloud-config",
        type=Path,
        default=Path("tools/cloud_config.env"),
    )
    return parser.parse_args()


def load_cases(path: Path) -> list[dict[str, Any]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, list):
        raise ValueError("task input corpus must be a JSON array")
    return payload


def differences(expected: dict[str, Any], actual: TaskRecord) -> list[str]:
    failures: list[str] = []
    for field in ("is_task", "time", "place", "event"):
        if field not in expected:
            continue
        actual_value = getattr(actual, field)
        if actual_value != expected[field]:
            failures.append(
                f"{field}: expected {expected[field]!r}, got {actual_value!r}"
            )
    return failures


def main() -> int:
    args = parse_args()
    load_env_file(args.cloud_config)
    if not os.environ.get("DEEPSEEK_API_KEY", "").strip():
        raise SystemExit("Fill DEEPSEEK_API_KEY in tools/cloud_config.env")

    client = DeepSeekClient(DeepSeekConfig.from_env())
    cases = load_cases(args.cases)
    passed = 0
    for index, case in enumerate(cases, start=1):
        transcript = case.get("transcript")
        if not isinstance(transcript, str) or not transcript.strip():
            raise ValueError(f"case {index} has no transcript")
        task = client.structure_transcript(transcript)
        failures = differences(case, task)
        status = "PASS" if not failures else "FAIL"
        print(f"[{status}] {transcript} -> {task}")
        for failure in failures:
            print(f"       {failure}")
        if not failures:
            passed += 1

    print(f"\nPassed {passed}/{len(cases)} cases")
    return 0 if passed == len(cases) else 1


if __name__ == "__main__":
    raise SystemExit(main())
