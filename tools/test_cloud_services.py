#!/usr/bin/env python3

import json
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlparse

from cloud_services import (
    BaiduConfig,
    BaiduSpeechClient,
    CloudServiceError,
    DeepSeekClient,
    DeepSeekConfig,
    TaskRecord,
    display_columns,
    format_task,
    normalize_time_text,
    parse_task_json,
)


class FakeResponse:
    def __init__(self, payload: object) -> None:
        self.body = json.dumps(payload, ensure_ascii=False).encode("utf-8")

    def read(self) -> bytes:
        return self.body

    def __enter__(self) -> "FakeResponse":
        return self

    def __exit__(self, *args: object) -> None:
        return None


class QueueOpener:
    def __init__(self, *payloads: object) -> None:
        self.payloads = list(payloads)
        self.calls: list[tuple[object, float]] = []

    def __call__(self, request: object, timeout: float) -> FakeResponse:
        self.calls.append((request, timeout))
        return FakeResponse(self.payloads.pop(0))


class CloudTaskParsingTest(unittest.TestCase):
    def test_parses_bounded_english_schema(self) -> None:
        task = parse_task_json(
            '{"time":"明天 9 点","place":"会议室","event":"确认接口"}'
        )
        self.assertEqual("明天 9 点", task.time)
        self.assertEqual("确认接口", task.event)

    def test_accepts_chinese_aliases_and_markdown_fence(self) -> None:
        task = parse_task_json(
            '```json\n{"时间":"今天","地点":"现场","事情":"检查设备"}\n```'
        )
        self.assertEqual("今天", task.time)
        self.assertEqual("检查设备", task.event)

    def test_limits_fields(self) -> None:
        task = parse_task_json(
            '{"time":"' + "t" * 80 + '","place":"","event":"e"}'
        )
        self.assertEqual(26, len(task.time))

    def test_limits_fields_by_rendered_width(self) -> None:
        task = parse_task_json(
            '{"time":"","place":"","event":"' + "继续写帖子" * 8 + '"}'
        )

        self.assertLessEqual(display_columns(task.event), 26)

    def test_formats_in_required_order(self) -> None:
        self.assertEqual(
            "时间：今天\n地点：会议室\n事情：确认接口",
            format_task(TaskRecord("今天", "会议室", "确认接口"), "原始文本"),
        )

    def test_formatted_lines_fit_the_display(self) -> None:
        rendered = format_task(
            TaskRecord("明天" * 20, "会议室" * 20, "继续完成开发任务" * 10)
        )

        self.assertEqual(3, len(rendered.splitlines()))
        self.assertTrue(
            all(display_columns(line) <= 32 for line in rendered.splitlines())
        )

    def test_falls_back_when_all_fields_are_empty(self) -> None:
        self.assertEqual("原始文本", format_task(TaskRecord(), "原始文本"))

    def test_missing_event_uses_transcript_as_the_task(self) -> None:
        self.assertEqual(
            "时间：明天\n地点：-\n事情：明天下午学习嵌入式开发",
            format_task(
                TaskRecord(time="明天"), "明天下午学习嵌入式开发"
            ),
        )

    def test_rejects_non_json(self) -> None:
        with self.assertRaises(CloudServiceError):
            parse_task_json("没有结构化结果")

    def test_non_task_clears_task_fields(self) -> None:
        task = parse_task_json(
            '{"is_task":false,"time":"今天","place":"",'
            '"event":"天气不错","reason":"只是陈述"}'
        )

        self.assertFalse(task.is_task)
        self.assertEqual("", task.time)
        self.assertEqual("", task.event)
        self.assertEqual("只是陈述", task.reason)

    def test_old_response_infers_task_from_fields(self) -> None:
        task = parse_task_json(
            '{"time":"","place":"","event":"学习嵌入式开发"}'
        )

        self.assertTrue(task.is_task)

    def test_normalizes_common_spoken_time_expressions(self) -> None:
        cases = {
            "一会儿": "稍后",
            "待会": "稍后",
            "等一下": "稍后",
            "今天晚上": "今晚",
            "下班以后": "下班后",
            "有空": "有空时",
        }

        for spoken, written in cases.items():
            with self.subTest(spoken=spoken):
                self.assertEqual(written, normalize_time_text(spoken))

    def test_parsed_task_uses_normalized_time(self) -> None:
        task = parse_task_json(
            '{"is_task":true,"time":"一会儿","place":"",'
            '"event":"维修手机","reason":""}'
        )

        self.assertEqual("稍后", task.time)


class BaiduSpeechClientContractTest(unittest.TestCase):
    def test_requests_token_then_submits_raw_pcm(self) -> None:
        opener = QueueOpener(
            {"access_token": "token-one", "expires_in": 3600},
            {"err_no": 0, "result": ["今天学习嵌入式开发"]},
        )
        client = BaiduSpeechClient(
            BaiduConfig(
                api_key="baidu-key",
                secret_key="baidu-secret",
                cuid="device-1",
                token_url="https://token.example/oauth",
                speech_url="https://speech.example/recognize",
                timeout=7,
            ),
            opener=opener,
        )

        self.assertEqual(
            "今天学习嵌入式开发",
            client.transcribe_pcm(b"\x01\x00\x02\x00"),
        )
        self.assertEqual(2, len(opener.calls))

        token_request, token_timeout = opener.calls[0]
        token_url = urlparse(token_request.full_url)  # type: ignore[attr-defined]
        token_query = parse_qs(token_url.query)
        self.assertEqual("POST", token_request.get_method())  # type: ignore[attr-defined]
        self.assertEqual(["baidu-key"], token_query["client_id"])
        self.assertEqual(["baidu-secret"], token_query["client_secret"])
        self.assertEqual(7, token_timeout)

        speech_request, _ = opener.calls[1]
        speech_url = urlparse(speech_request.full_url)  # type: ignore[attr-defined]
        speech_query = parse_qs(speech_url.query)
        self.assertEqual(["token-one"], speech_query["token"])
        self.assertEqual(["device-1"], speech_query["cuid"])
        self.assertEqual(b"\x01\x00\x02\x00", speech_request.data)  # type: ignore[attr-defined]
        self.assertEqual(
            "audio/pcm;rate=16000",
            speech_request.get_header("Content-type"),  # type: ignore[attr-defined]
        )

    def test_refreshes_token_before_provider_expiry(self) -> None:
        opener = QueueOpener(
            {"access_token": "token-one", "expires_in": 120},
            {"access_token": "token-two", "expires_in": 120},
        )
        client = BaiduSpeechClient(
            BaiduConfig(api_key="key", secret_key="secret"), opener=opener
        )

        with patch("cloud_services.time.time", return_value=1000):
            self.assertEqual("token-one", client.get_access_token())
        with patch("cloud_services.time.time", return_value=1059):
            self.assertEqual("token-one", client.get_access_token())
        with patch("cloud_services.time.time", return_value=1060):
            self.assertEqual("token-two", client.get_access_token())

        self.assertEqual(2, len(opener.calls))


class DeepSeekClientContractTest(unittest.TestCase):
    def test_sends_chat_request_and_parses_task(self) -> None:
        opener = QueueOpener(
            {
                "choices": [
                    {
                        "message": {
                            "content": json.dumps(
                                {
                                    "is_task": True,
                                    "time": "今天下午",
                                    "place": "",
                                    "event": "学习嵌入式开发",
                                    "reason": "",
                                },
                                ensure_ascii=False,
                            )
                        }
                    }
                ]
            }
        )
        client = DeepSeekClient(
            DeepSeekConfig(
                api_key="deepseek-key",
                model="deepseek-test",
                api_url="https://deepseek.example/chat",
                timeout=9,
            ),
            opener=opener,
        )

        task = client.structure_transcript("今天下午学习嵌入式开发")

        self.assertTrue(task.is_task)
        self.assertEqual("今天下午", task.time)
        self.assertEqual("学习嵌入式开发", task.event)
        request, timeout = opener.calls[0]
        self.assertEqual("POST", request.get_method())  # type: ignore[attr-defined]
        self.assertEqual(  # type: ignore[attr-defined]
            "Bearer deepseek-key", request.get_header("Authorization")
        )
        body = json.loads(request.data.decode("utf-8"))  # type: ignore[attr-defined]
        self.assertEqual("deepseek-test", body["model"])
        prompt = body["messages"][1]["content"]
        self.assertIn("今天下午学习嵌入式开发", prompt)
        self.assertIn("第一人称", prompt)
        self.assertIn("回家要吃饭", prompt)
        self.assertIn("一会儿要去修手机", prompt)
        self.assertIn("稍后", prompt)
        self.assertNotIn("person", prompt)
        self.assertEqual(9, timeout)


if __name__ == "__main__":
    unittest.main()
