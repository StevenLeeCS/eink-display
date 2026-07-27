#!/usr/bin/env python3

from io import BytesIO
import unittest

from audio_receiver import (
    AudioTooLargeError,
    AudioUploadError,
    copy_chunked_audio,
    speech_detection_header,
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


if __name__ == "__main__":
    unittest.main()
