#!/usr/bin/env python3
"""Provider-independent recognition and task-display orchestration."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
import re
import threading
from typing import Any, Protocol
import unicodedata
import wave

from cloud_services import (
    BaiduSpeechClient,
    CloudServiceError,
    DISPLAY_COLUMNS_PER_LINE,
    DeepSeekClient,
    TaskRecord,
    format_task,
)
from task_schedule import TaskSchedule, resolve_task_schedule


NO_SPEECH_PROMPT = "未识别到语音"
NOT_TASK_EVENT = "[未识别到待办事项,请重试]"
DISPLAY_LINE_COUNT = 3
PRODUCT_TIMEZONE = timezone(timedelta(hours=8), "Asia/Shanghai")

PUNCTUATION_TRANSLATION = str.maketrans(
    {
        "，": ",",
        "。": ".",
        "！": "!",
        "？": "?",
        "：": ":",
        "；": ";",
        "（": "(",
        "）": ")",
        "“": '"',
        "”": '"',
        "‘": "'",
        "’": "'",
    }
)


class TextConverter(Protocol):
    def convert(self, text: str) -> str: ...


@dataclass(frozen=True)
class RecognitionResult:
    text: str
    speech_detected: bool
    task_detected: bool
    schedule: TaskSchedule = TaskSchedule()


def fit_display_text(text: str) -> str:
    """Wrap and cap output to the e-paper's three 32-column text lines."""

    lines: list[str] = []
    current: list[str] = []
    columns = 0

    def finish_line() -> bool:
        nonlocal current, columns
        lines.append("".join(current).strip())
        current = []
        columns = 0
        return len(lines) >= DISPLAY_LINE_COUNT

    for character in text.strip():
        if character == "\r":
            continue
        if character == "\n":
            if finish_line():
                break
            continue
        width = 0 if unicodedata.combining(character) else (
            2 if unicodedata.east_asian_width(character) in {"F", "W"} else 1
        )
        if current and columns + width > DISPLAY_COLUMNS_PER_LINE:
            if finish_line():
                break
        if columns + width <= DISPLAY_COLUMNS_PER_LINE:
            current.append(character)
            columns += width

    if current and len(lines) < DISPLAY_LINE_COUNT:
        lines.append("".join(current).strip())
    return "\n".join(line for line in lines if line)


def structure_for_display(
    client: DeepSeekClient, transcript: str
) -> tuple[str, bool]:
    task = client.structure_transcript(transcript)
    return task_for_display(task, transcript)


def task_for_display(task: TaskRecord, transcript: str) -> tuple[str, bool]:
    if not task.is_task:
        return format_task(TaskRecord(event=NOT_TASK_EVENT), transcript), False
    return format_task(task, transcript), True


_CHINESE_DIGITS = {
    "零": 0,
    "一": 1,
    "二": 2,
    "两": 2,
    "三": 3,
    "四": 4,
    "五": 5,
    "六": 6,
    "七": 7,
    "八": 8,
    "九": 9,
}


def _chinese_integer(value: str) -> int | None:
    if value.isdigit():
        return int(value)
    total = 0
    section = 0
    digit = 0
    for character in value:
        if character in _CHINESE_DIGITS:
            digit = _CHINESE_DIGITS[character]
        elif character == "十":
            section += (digit or 1) * 10
            digit = 0
        elif character == "百":
            section += (digit or 1) * 100
            digit = 0
        else:
            return None
    total += section + digit
    return total if total > 0 else None


def parse_pomodoro_minutes(transcript: str) -> tuple[int | None, int | None]:
    """Extract explicitly labelled focus/rest minute values from speech."""

    compact = re.sub(r"\s+", "", transcript)

    def value_after(keyword: str) -> int | None:
        match = re.search(
            rf"{keyword}(?:时间)?(?:改成|设置为|设为|是|为)?"
            r"([0-9零一二两三四五六七八九十百]+)(?:分钟|分)",
            compact,
        )
        return _chinese_integer(match.group(1)) if match else None

    return value_after("专注"), value_after("休息")


class RecognitionPipeline:
    """Run one WAV recording through STT, task structuring and text cleanup."""

    def __init__(
        self,
        stt_provider: str,
        *,
        transcriber: Any | None = None,
        baidu_client: BaiduSpeechClient | None = None,
        deepseek_client: DeepSeekClient | None = None,
        converter: TextConverter | None = None,
        language: str = "zh",
        debug: bool = False,
    ) -> None:
        if stt_provider not in {"local", "baidu"}:
            raise ValueError("stt_provider must be 'local' or 'baidu'")
        self.stt_provider = stt_provider
        self.transcriber = transcriber
        self.baidu_client = baidu_client
        self.deepseek_client = deepseek_client
        self.converter = converter
        self.language = language
        self.debug = debug
        self._lock = threading.Lock()

    def recognize(
        self,
        wav_path: Path,
        *,
        request_id: str | None = None,
        structure_task: bool = True,
    ) -> RecognitionResult:
        with self._lock:
            transcript = self._transcribe(wav_path)
            log_id = request_id or "unknown"
            if self.debug:
                print(
                    f"[recognition:{log_id}] transcript={transcript}",
                    flush=True,
                )
            speech_detected = bool(transcript)
            task_detected = speech_detected
            display_text = transcript
            schedule = TaskSchedule()

            if (
                speech_detected
                and structure_task
                and self.deepseek_client is not None
            ):
                try:
                    current_time = datetime.now(PRODUCT_TIMEZONE)
                    task = self.deepseek_client.structure_transcript(
                        transcript,
                        current_time=current_time.isoformat(timespec="minutes"),
                    )
                    if self.debug:
                        print(
                            f"[recognition:{log_id}] task="
                            + json.dumps(asdict(task), ensure_ascii=False),
                            flush=True,
                        )
                    display_text, task_detected = task_for_display(
                        task, transcript
                    )
                    if task_detected:
                        schedule = resolve_task_schedule(task.time, current_time)
                    print(
                        f"Structured result (task={task_detected}): {display_text}",
                        flush=True,
                    )
                except CloudServiceError as error:
                    print(
                        "WARNING: DeepSeek processing failed; "
                        f"using transcript: {error}",
                        flush=True,
                    )

            if not speech_detected:
                display_text = NO_SPEECH_PROMPT
            elif self.converter is not None:
                display_text = self.converter.convert(display_text)
            display_text = display_text.translate(PUNCTUATION_TRANSLATION)
            display_text = fit_display_text(display_text)

            return RecognitionResult(
                text=display_text,
                speech_detected=speech_detected,
                task_detected=task_detected,
                schedule=schedule,
            )

    def _transcribe(self, wav_path: Path) -> str:
        if self.stt_provider == "baidu":
            if self.baidu_client is None:
                raise CloudServiceError("Baidu STT client is not configured")
            with wave.open(str(wav_path), "rb") as wav_file:
                pcm_data = wav_file.readframes(wav_file.getnframes())
                sample_rate = wav_file.getframerate()
            return self.baidu_client.transcribe_pcm(pcm_data, sample_rate)

        if self.transcriber is None:
            raise CloudServiceError("local faster-whisper is not configured")
        segments, _ = self.transcriber.transcribe(
            str(wav_path),
            language=self.language,
            beam_size=5,
            vad_filter=True,
            condition_on_previous_text=False,
        )
        return "".join(segment.text for segment in segments).strip()


__all__ = [
    "NOT_TASK_EVENT",
    "NO_SPEECH_PROMPT",
    "RecognitionPipeline",
    "RecognitionResult",
    "fit_display_text",
    "parse_pomodoro_minutes",
    "structure_for_display",
    "task_for_display",
]
