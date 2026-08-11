#!/usr/bin/env python3
"""Deterministic schedule coordinates for common Chinese task times."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import date, datetime, time, timedelta, timezone
import re


PRODUCT_TIMEZONE = timezone(timedelta(hours=8), "Asia/Shanghai")


@dataclass(frozen=True)
class TaskSchedule:
    kind: str = "none"
    start_at: int = 0
    end_at: int = 0


TIME_WINDOWS = (
    ("凌晨", 0, 6),
    ("早上", 6, 9),
    ("上午", 9, 12),
    ("中午", 12, 14),
    ("下午", 14, 18),
    ("傍晚", 18, 20),
    ("晚上", 20, 23),
    ("今晚", 20, 23),
    ("深夜", 23, 24),
)

UNSCHEDULED_EXPRESSIONS = (
    "稍后",
    "有空",
    "空闲",
    "下班后",
    "改天",
    "过会",
)


def _resolved_date(text: str, today: date) -> tuple[date, bool]:
    if "后天" in text:
        return today + timedelta(days=2), True
    if "明天" in text or "明日" in text:
        return today + timedelta(days=1), True
    if "今天" in text or "今日" in text or "今晚" in text:
        return today, True

    match = re.search(r"(?:(\d{4})年)?(\d{1,2})月(\d{1,2})[日号]?", text)
    if match:
        year = int(match.group(1) or today.year)
        try:
            candidate = date(year, int(match.group(2)), int(match.group(3)))
        except ValueError:
            return today, False
        if match.group(1) is None and candidate < today:
            candidate = date(year + 1, candidate.month, candidate.day)
        return candidate, True
    return today, False


def _timestamp(day: date, hour: int, minute: int, now: datetime) -> int:
    if hour == 24:
        day += timedelta(days=1)
        hour = 0
    return int(datetime.combine(day, time(hour, minute), now.tzinfo).timestamp())


def resolve_task_schedule(
    text: str, now: datetime | None = None
) -> TaskSchedule:
    value = "".join(text.split())
    if not value or any(item in value for item in UNSCHEDULED_EXPRESSIONS):
        return TaskSchedule()

    current = now or datetime.now(PRODUCT_TIMEZONE)
    if current.tzinfo is None:
        current = current.astimezone()
    day, has_date = _resolved_date(value, current.date())

    exact = re.search(r"(?<!\d)([01]?\d|2[0-3])[:：]([0-5]\d)(?!\d)", value)
    if exact:
        hour, minute = int(exact.group(1)), int(exact.group(2))
        point = _timestamp(day, hour, minute, current)
        return TaskSchedule("exact", point, point)

    exact = re.search(r"(?<!\d)(\d{1,2})[点时](?:(\d{1,2})分?|半)?", value)
    if exact:
        hour = int(exact.group(1))
        minute_text = exact.group(2)
        minute = int(minute_text) if minute_text else (30 if "半" in exact.group(0) else 0)
        if hour > 23 or minute > 59:
            return TaskSchedule()
        if any(period in value for period in ("下午", "傍晚", "晚上", "今晚")):
            if hour < 12:
                hour += 12
        elif "中午" in value and hour < 11:
            hour += 12
        point = _timestamp(day, hour, minute, current)
        return TaskSchedule("exact", point, point)

    for label, start_hour, end_hour in TIME_WINDOWS:
        if label in value:
            return TaskSchedule(
                "window",
                _timestamp(day, start_hour, 0, current),
                _timestamp(day, end_hour, 0, current),
            )

    if has_date:
        return TaskSchedule(
            "window",
            _timestamp(day, 6, 0, current),
            _timestamp(day, 22, 0, current),
        )
    return TaskSchedule()


__all__ = ["TaskSchedule", "resolve_task_schedule"]
