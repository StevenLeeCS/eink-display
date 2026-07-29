#!/usr/bin/env python3

from io import BytesIO
from pathlib import Path
from types import SimpleNamespace
import unittest

from audio_receiver import (
    AudioTooLargeError,
    AudioUploadError,
    copy_chunked_audio,
    speech_detection_header,
    task_detection_header,
)
from cloud_services import CloudServiceError, TaskRecord
from recognition_pipeline import RecognitionPipeline, structure_for_display


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

    def test_deepseek_client_contract_is_used(self) -> None:
        class FakeDeepSeekClient:
            def structure_transcript(self, transcript: str) -> TaskRecord:
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

    def test_non_task_returns_marker_free_prompt(self) -> None:
        class FakeDeepSeekClient:
            def structure_transcript(self, transcript: str) -> TaskRecord:
                return TaskRecord(is_task=False, reason="只是闲聊")

        displayed, task_detected = structure_for_display(
            FakeDeepSeekClient(), "今天天气不错"  # type: ignore[arg-type]
        )

        self.assertFalse(task_detected)
        self.assertEqual("未识别到待办事项", displayed)


class RecognitionPipelineTest(unittest.TestCase):
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
            def structure_transcript(self, transcript: str) -> TaskRecord:
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


if __name__ == "__main__":
    unittest.main()
