#!/usr/bin/env python3

from datetime import datetime, timedelta, timezone
import unittest

from task_schedule import resolve_task_schedule


CHINA_TIME = timezone(timedelta(hours=8))


class TaskScheduleTest(unittest.TestCase):
    def setUp(self) -> None:
        self.now = datetime(2026, 8, 7, 13, 0, tzinfo=CHINA_TIME)

    def test_maps_common_period_to_fixed_window(self) -> None:
        schedule = resolve_task_schedule("今天下午", self.now)

        self.assertEqual("window", schedule.kind)
        self.assertEqual(
            datetime(2026, 8, 7, 14, 0, tzinfo=CHINA_TIME).timestamp(),
            schedule.start_at,
        )
        self.assertEqual(
            datetime(2026, 8, 7, 18, 0, tzinfo=CHINA_TIME).timestamp(),
            schedule.end_at,
        )

    def test_resolves_relative_day_before_period(self) -> None:
        schedule = resolve_task_schedule("明天上午", self.now)

        self.assertEqual(
            datetime(2026, 8, 8, 9, 0, tzinfo=CHINA_TIME).timestamp(),
            schedule.start_at,
        )
        self.assertEqual(
            datetime(2026, 8, 8, 12, 0, tzinfo=CHINA_TIME).timestamp(),
            schedule.end_at,
        )

    def test_resolves_exact_period_time(self) -> None:
        schedule = resolve_task_schedule("明天晚上8点半", self.now)

        expected = datetime(2026, 8, 8, 20, 30, tzinfo=CHINA_TIME).timestamp()
        self.assertEqual("exact", schedule.kind)
        self.assertEqual(expected, schedule.start_at)
        self.assertEqual(expected, schedule.end_at)

    def test_date_without_time_uses_day_window(self) -> None:
        schedule = resolve_task_schedule("后天", self.now)

        self.assertEqual(
            datetime(2026, 8, 9, 6, 0, tzinfo=CHINA_TIME).timestamp(),
            schedule.start_at,
        )
        self.assertEqual(
            datetime(2026, 8, 9, 22, 0, tzinfo=CHINA_TIME).timestamp(),
            schedule.end_at,
        )

    def test_rolls_past_month_day_to_next_year(self) -> None:
        schedule = resolve_task_schedule("7月1日上午", self.now)

        self.assertEqual(
            datetime(2027, 7, 1, 9, 0, tzinfo=CHINA_TIME).timestamp(),
            schedule.start_at,
        )

    def test_leaves_unreliable_relative_phrases_unscheduled(self) -> None:
        for text in ("稍后", "有空时", "下班后"):
            with self.subTest(text=text):
                self.assertEqual("none", resolve_task_schedule(text, self.now).kind)

    def test_leaves_text_without_time_unscheduled(self) -> None:
        self.assertEqual("none", resolve_task_schedule("去图书馆", self.now).kind)


if __name__ == "__main__":
    unittest.main()
