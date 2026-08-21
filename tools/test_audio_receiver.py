#!/usr/bin/env python3

from contextlib import redirect_stdout
from io import BytesIO, StringIO
from pathlib import Path
from types import SimpleNamespace
import unittest

from audio_receiver import (
    AudioTooLargeError,
    AudioUploadError,
    copy_chunked_audio,
    should_retain_recording,
    speech_detection_header,
    task_detection_header,
)
from cloud_services import CloudServiceError, TaskRecord, display_columns
from recognition_pipeline import (
    RecognitionPipeline,
    RecognitionResult,
    fit_display_text,
    parse_pomodoro_minutes,
    structure_for_display,
)


class ChunkedAudioTest(unittest.TestCase):
    def test_combines_chunks(self) -> None:
        output = bytearray()
        received = copy_chunked_audio(
            BytesIO(b"4\r\nabcd\r\n2\r\nef\r\n0\r\n\r\n"),
            output.extend,
        )

        self.assertEqual(6, received)
        self.assertEqual(b"abcdef", output)

    def test_rejects_invalid_terminator(self) -> None:
        with self.assertRaises(AudioUploadError):
            copy_chunked_audio(BytesIO(b"1\r\naX"), lambda _: None)

    def test_rejects_oversized_chunk_before_reading_it(self) -> None:
        with self.assertRaises(AudioTooLargeError):
            copy_chunked_audio(BytesIO(b"200001\r\n"), lambda _: None)

    def test_reports_chunked_trailer(self) -> None:
        trailers: list[tuple[bytes, bytes]] = []
        received = copy_chunked_audio(
            BytesIO(b"2\r\nab\r\n0\r\nX-Discard-Audio: 1\r\n\r\n"),
            lambda _: None,
            lambda name, value: trailers.append((name, value)),
        )

        self.assertEqual(2, received)
        self.assertEqual([(b"x-discard-audio", b"1")], trailers)

    def test_empty_recognition_is_not_speech(self) -> None:
        self.assertEqual("0", speech_detection_header(False))

    def test_nonempty_recognition_is_speech(self) -> None:
        self.assertEqual("1", speech_detection_header(True))

    def test_task_detection_header(self) -> None:
        self.assertEqual("1", task_detection_header(True))
        self.assertEqual("0", task_detection_header(False))

    def test_failed_audio_retention_is_explicit_and_selective(self) -> None:
        task = RecognitionResult("任务", True, True)
        not_task = RecognitionResult("未识别到待办事项", True, False)
        no_speech = RecognitionResult("未识别到语音", False, False)

        self.assertFalse(should_retain_recording(False, not_task))
        self.assertFalse(should_retain_recording(True, task))
        self.assertTrue(should_retain_recording(True, not_task))
        self.assertTrue(should_retain_recording(True, no_speech))
        self.assertTrue(
            should_retain_recording(True, None, recognition_failed=True)
        )

    def test_deepseek_client_contract_is_used(self) -> None:
        class FakeDeepSeekClient:
            def structure_transcript(
                self, transcript: str, **kwargs: object
            ) -> TaskRecord:
                self.transcript = transcript
                return TaskRecord(time="今天", event="学习嵌入式开发")

        client = FakeDeepSeekClient()
        displayed, task_detected = structure_for_display(
            client, "今天学习嵌入式开发"  # type: ignore[arg-type]
        )

        self.assertEqual("今天学习嵌入式开发", client.transcript)
        self.assertTrue(task_detected)
        self.assertIn("时间：今天", displayed)
        self.assertIn("事情：学习嵌入式开发", displayed)

    def test_non_task_returns_normal_task_display_structure(self) -> None:
        class FakeDeepSeekClient:
            def structure_transcript(
                self, transcript: str, **kwargs: object
            ) -> TaskRecord:
                return TaskRecord(is_task=False, reason="只是闲聊")

        displayed, task_detected = structure_for_display(
            FakeDeepSeekClient(), "今天天气不错"  # type: ignore[arg-type]
        )

        self.assertFalse(task_detected)
        self.assertEqual(
            "时间：-\n地点：-\n事情：[未识别到待办事项,请重试]",
            displayed,
        )


class RecognitionPipelineTest(unittest.TestCase):
    def test_parses_pomodoro_minutes(self) -> None:
        self.assertEqual(
            (25, 5), parse_pomodoro_minutes("专注二十五分钟,休息5分钟")
        )
        self.assertEqual(
            (40, None), parse_pomodoro_minutes("专注时间改成40分钟")
        )
        self.assertEqual(
            (None, 10), parse_pomodoro_minutes("休息设为十分钟")
        )

    def test_rejects_unlabelled_pomodoro_duration(self) -> None:
        self.assertEqual((None, None), parse_pomodoro_minutes("设置二十五分钟"))

    def test_display_text_is_limited_to_three_lines(self) -> None:
        displayed = fit_display_text("一" * 60)

        lines = displayed.splitlines()
        self.assertEqual(3, len(lines))
        self.assertTrue(all(len(line) == 16 for line in lines))

    def test_local_transcript_is_cleaned_for_display(self) -> None:
        class FakeTranscriber:
            def transcribe(self, *args: object, **kwargs: object) -> tuple[list[object], object]:
                return [SimpleNamespace(text="今天，学习嵌入式开发。")], object()

        pipeline = RecognitionPipeline("local", transcriber=FakeTranscriber())

        result = pipeline.recognize(Path("unused.wav"))

        self.assertEqual("今天,学习嵌入式开发.", result.text)
        self.assertTrue(result.speech_detected)
        self.assertTrue(result.task_detected)

    def test_deepseek_failure_preserves_transcript(self) -> None:
        class FakeTranscriber:
            def transcribe(self, *args: object, **kwargs: object) -> tuple[list[object], object]:
                return [SimpleNamespace(text="今天学习嵌入式开发")], object()

        class FailingDeepSeekClient:
            def structure_transcript(
                self, transcript: str, **kwargs: object
            ) -> TaskRecord:
                raise CloudServiceError("temporary failure")

        pipeline = RecognitionPipeline(
            "local",
            transcriber=FakeTranscriber(),
            deepseek_client=FailingDeepSeekClient(),  # type: ignore[arg-type]
        )

        result = pipeline.recognize(Path("unused.wav"))

        self.assertEqual("今天学习嵌入式开发", result.text)
        self.assertTrue(result.speech_detected)
        self.assertTrue(result.task_detected)

    def test_non_task_pipeline_uses_supported_punctuation_and_fits(self) -> None:
        class FakeTranscriber:
            def transcribe(
                self, *args: object, **kwargs: object
            ) -> tuple[list[object], object]:
                return [SimpleNamespace(text="今天天气不错")], object()

        class FakeDeepSeekClient:
            def structure_transcript(
                self, transcript: str, **kwargs: object
            ) -> TaskRecord:
                return TaskRecord(is_task=False, reason="闲聊")

        pipeline = RecognitionPipeline(
            "local",
            transcriber=FakeTranscriber(),
            deepseek_client=FakeDeepSeekClient(),  # type: ignore[arg-type]
        )

        result = pipeline.recognize(Path("unused.wav"))

        self.assertEqual(
            "时间:-\n地点:-\n事情:[未识别到待办事项,请重试]",
            result.text,
        )
        self.assertFalse(result.task_detected)
        self.assertTrue(
            all(display_columns(line) <= 32 for line in result.text.splitlines())
        )

    def test_debug_log_separates_transcript_and_task_decision(self) -> None:
        class FakeTranscriber:
            def transcribe(
                self, *args: object, **kwargs: object
            ) -> tuple[list[object], object]:
                return [SimpleNamespace(text="回家要吃饭")], object()

        class FakeDeepSeekClient:
            def structure_transcript(
                self, transcript: str, **kwargs: object
            ) -> TaskRecord:
                return TaskRecord(
                    time="",
                    place="家",
                    event="吃饭",
                    is_task=True,
                    reason="未来行动意图",
                )

        pipeline = RecognitionPipeline(
            "local",
            transcriber=FakeTranscriber(),
            deepseek_client=FakeDeepSeekClient(),  # type: ignore[arg-type]
            debug=True,
        )
        output = StringIO()

        with redirect_stdout(output):
            result = pipeline.recognize(Path("unused.wav"), request_id="case-1")

        log = output.getvalue()
        self.assertIn("[recognition:case-1] transcript=回家要吃饭", log)
        self.assertIn('"is_task": true', log)
        self.assertIn('"reason": "未来行动意图"', log)
        self.assertTrue(result.task_detected)

if __name__ == "__main__":
    unittest.main()
