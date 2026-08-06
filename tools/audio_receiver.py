#!/usr/bin/env python3
"""Receive PCM through a temporary WAV file and return recognized speech."""

from __future__ import annotations

import argparse
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
from typing import BinaryIO, Callable
import wave

try:
    from faster_whisper import WhisperModel
except ModuleNotFoundError:
    WhisperModel = None  # type: ignore[assignment,misc]

try:
    from opencc import OpenCC
except ModuleNotFoundError as error:
    raise SystemExit(
        "opencc is not installed. Run: "
        r".\.venv\Scripts\python.exe -m pip install -r requirements.txt"
    ) from error

from cloud_services import (
    BaiduConfig,
    BaiduSpeechClient,
    CloudServiceError,
    DeepSeekConfig,
    DeepSeekClient,
)
from recognition_pipeline import RecognitionPipeline, RecognitionResult


MAX_RECORDING_SECONDS = 60
PCM_BYTES_PER_SECOND = 16000 * 1 * 2
MAX_AUDIO_BYTES = MAX_RECORDING_SECONDS * PCM_BYTES_PER_SECOND


class AudioUploadError(Exception):
    status = 400


class AudioTooLargeError(AudioUploadError):
    status = 413


def speech_detection_header(speech_detected: bool) -> str:
    return "1" if speech_detected else "0"


def task_detection_header(task_detected: bool) -> str:
    return "1" if task_detected else "0"


def env_flag(name: str, default: bool = False) -> bool:
    fallback = "true" if default else "false"
    value = os.environ.get(name, fallback).strip().lower()
    return value in {"1", "true", "yes"}


def should_retain_recording(
    keep_failed_audio: bool,
    result: RecognitionResult | None,
    *,
    recognition_failed: bool = False,
) -> bool:
    if not keep_failed_audio:
        return False
    return (
        recognition_failed
        or result is None
        or not result.speech_detected
        or not result.task_detected
    )


def finalize_recording(path: Path, retain: bool) -> None:
    if retain:
        print(f"Retained diagnostic recording: {path}", flush=True)
        return
    try:
        path.unlink(missing_ok=True)
        print(f"Deleted temporary recording: {path.name}", flush=True)
    except OSError as error:
        print(f"WARNING: Could not delete {path.name}: {error}", flush=True)


def _read_exact(stream: BinaryIO, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = stream.read(size - len(data))
        if not chunk:
            raise AudioUploadError("audio upload ended early")
        data.extend(chunk)
    return bytes(data)


def copy_chunked_audio(
    stream: BinaryIO,
    write_chunk: Callable[[bytes], object],
    receive_trailer: Callable[[bytes, bytes], object] | None = None,
) -> int:
    total = 0
    while True:
        size_line = stream.readline(128)
        if not size_line or not size_line.endswith(b"\n"):
            raise AudioUploadError("invalid chunk header")
        size_token = size_line.split(b";", 1)[0].strip()
        try:
            chunk_size = int(size_token, 16)
        except ValueError as error:
            raise AudioUploadError("invalid chunk size") from error

        if chunk_size == 0:
            while True:
                trailer = stream.readline(8192)
                if trailer in (b"\r\n", b"\n"):
                    return total
                if not trailer:
                    raise AudioUploadError("incomplete chunk trailer")
                if receive_trailer is not None and b":" in trailer:
                    name, value = trailer.split(b":", 1)
                    receive_trailer(name.strip().lower(), value.strip())

        if total + chunk_size > MAX_AUDIO_BYTES:
            raise AudioTooLargeError("audio exceeds maximum size")
        chunk = _read_exact(stream, chunk_size)
        if _read_exact(stream, 2) != b"\r\n":
            raise AudioUploadError("invalid chunk terminator")
        write_chunk(chunk)
        total += chunk_size


class AudioReceiverServer(ThreadingHTTPServer):
    daemon_threads = True
    output_dir: Path
    recognition_pipeline: RecognitionPipeline
    keep_failed_audio: bool


class AudioReceiverHandler(BaseHTTPRequestHandler):
    server_version = "EinkAudioReceiver/3.0"

    def do_POST(self) -> None:
        if self.path != "/audio":
            self.send_error(404, "Use POST /audio")
            return

        try:
            sample_rate = int(self.headers.get("X-Sample-Rate", "16000"))
            channels = int(self.headers.get("X-Channels", "1"))
            sample_width = int(self.headers.get("X-Sample-Width", "2"))
        except (TypeError, ValueError):
            self.send_error(400, "Invalid audio headers")
            return

        if sample_rate != 16000 or channels != 1 or sample_width != 2:
            self.send_error(400, "Expected 16 kHz, mono, signed PCM16")
            return

        transfer_encoding = self.headers.get("Transfer-Encoding", "").lower()
        chunked = transfer_encoding == "chunked"
        if transfer_encoding and not chunked:
            self.send_error(501, "Only chunked transfer encoding is supported")
            return

        content_length: int | None = None
        if not chunked:
            try:
                content_length = int(self.headers["Content-Length"])
            except (KeyError, TypeError, ValueError):
                self.send_error(411, "Content-Length or chunked encoding required")
                return
            if not 0 < content_length <= MAX_AUDIO_BYTES:
                self.send_error(413, "Invalid audio size")
                return

        server: AudioReceiverServer = self.server  # type: ignore[assignment]
        output_dir = server.output_dir
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
        final_path = output_dir / f"recording_{timestamp}.wav"
        partial_path = output_dir / f".recording_{timestamp}.partial"
        discard_audio = False

        def receive_trailer(name: bytes, value: bytes) -> None:
            nonlocal discard_audio
            if name == b"x-discard-audio" and value == b"1":
                discard_audio = True

        try:
            with wave.open(str(partial_path), "wb") as wav_file:
                wav_file.setnchannels(channels)
                wav_file.setsampwidth(sample_width)
                wav_file.setframerate(sample_rate)
                if chunked:
                    received_bytes = copy_chunked_audio(
                        self.rfile, wav_file.writeframesraw, receive_trailer
                    )
                else:
                    assert content_length is not None
                    remaining = content_length
                    received_bytes = 0
                    while remaining:
                        chunk = _read_exact(self.rfile, min(8192, remaining))
                        wav_file.writeframesraw(chunk)
                        remaining -= len(chunk)
                        received_bytes += len(chunk)
                if received_bytes == 0:
                    raise AudioUploadError("empty audio upload")
                if received_bytes % (channels * sample_width) != 0:
                    raise AudioUploadError("audio is not sample-aligned")
            partial_path.replace(final_path)
        except AudioUploadError as error:
            partial_path.unlink(missing_ok=True)
            self.send_error(error.status, str(error))
            return
        except (OSError, wave.Error) as error:
            partial_path.unlink(missing_ok=True)
            self.send_error(400, str(error))
            return

        if discard_audio:
            final_path.unlink(missing_ok=True)
            print("Discarded locally classified silent recording", flush=True)
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.send_header("X-Speech-Detected", "0")
            self.send_header("X-Task-Detected", "0")
            self.end_headers()
            return

        print(f"Saved {final_path} ({received_bytes} PCM bytes)", flush=True)
        print(
            "Recognizing speech with "
            f"{server.recognition_pipeline.stt_provider}...",
            flush=True,
        )
        try:
            result = server.recognition_pipeline.recognize(
                final_path, request_id=timestamp
            )
        except CloudServiceError as error:
            finalize_recording(
                final_path,
                should_retain_recording(
                    server.keep_failed_audio, None, recognition_failed=True
                ),
            )
            self.send_error(500, f"Speech recognition failed: {error}")
            return
        except Exception as error:  # The client needs a clear HTTP failure.
            finalize_recording(
                final_path,
                should_retain_recording(
                    server.keep_failed_audio, None, recognition_failed=True
                ),
            )
            self.send_error(500, f"Speech recognition failed: {error}")
            return

        finalize_recording(
            final_path,
            should_retain_recording(server.keep_failed_audio, result),
        )

        print(f"Recognized: {result.text}", flush=True)

        body = result.text.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header(
            "X-Speech-Detected", speech_detection_header(result.speech_detected)
        )
        self.send_header(
            "X-Task-Detected", task_detection_header(result.task_detected)
        )
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format_string: str, *args: object) -> None:
        print(f"{self.client_address[0]} - {format_string % args}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Receive ESP32 PCM through temporary WAV files and recognize speech."
    )
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=18000)
    parser.add_argument("--output", type=Path, default=Path("recordings"))
    parser.add_argument("--model", default="small")
    parser.add_argument("--device", default="cpu")
    parser.add_argument("--compute-type", default="int8")
    parser.add_argument("--language", default="zh")
    parser.add_argument(
        "--cloud-config",
        type=Path,
        default=Path("tools/cloud_config.env"),
        help="optional KEY=VALUE cloud API configuration file",
    )
    return parser.parse_args()


def load_env_file(path: Path) -> None:
    if not path.exists():
        return
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip("'\"")
        if key:
            os.environ.setdefault(key, value)


def main() -> None:
    args = parse_args()
    load_env_file(args.cloud_config)
    args.output.mkdir(parents=True, exist_ok=True)
    stt_provider = os.environ.get("STT_PROVIDER", "local").strip().lower()
    if stt_provider not in {"local", "baidu"}:
        raise SystemExit("STT_PROVIDER must be 'local' or 'baidu'")

    transcriber: WhisperModel | None = None
    baidu_client: BaiduSpeechClient | None = None
    if stt_provider == "baidu":
        if not os.environ.get("BAIDU_API_KEY", "").strip() or not os.environ.get(
            "BAIDU_SECRET_KEY", ""
        ).strip():
            raise SystemExit(
                "Fill BAIDU_API_KEY and BAIDU_SECRET_KEY in tools/cloud_config.env"
            )
        baidu_client = BaiduSpeechClient(BaiduConfig.from_env())
    else:
        if WhisperModel is None:
            raise SystemExit(
                "faster-whisper is not installed. Set STT_PROVIDER=baidu or install requirements."
            )
        print(
            f"Loading Whisper model {args.model} "
            f"({args.device}/{args.compute_type})...",
            flush=True,
        )
        transcriber = WhisperModel(
            args.model, device=args.device, compute_type=args.compute_type
        )

    deepseek_client: DeepSeekClient | None = None
    if env_flag("ENABLE_DEEPSEEK"):
        deepseek_key = os.environ.get("DEEPSEEK_API_KEY", "").strip()
        if deepseek_key:
            deepseek_client = DeepSeekClient(DeepSeekConfig.from_env())
        else:
            print(
                "WARNING: ENABLE_DEEPSEEK is true but DEEPSEEK_API_KEY is empty; "
                "structured processing is disabled.",
                flush=True,
            )

    server = AudioReceiverServer((args.host, args.port), AudioReceiverHandler)
    server.output_dir = args.output.resolve()
    server.keep_failed_audio = env_flag("KEEP_FAILED_AUDIO")
    server.recognition_pipeline = RecognitionPipeline(
        stt_provider,
        transcriber=transcriber,
        baidu_client=baidu_client,
        deepseek_client=deepseek_client,
        converter=OpenCC("t2s"),
        language=args.language,
        debug=env_flag("DEBUG_RECOGNITION"),
    )
    print(f"Listening on http://{args.host}:{args.port}/audio", flush=True)
    print(f"Temporary WAV directory: {server.output_dir}", flush=True)
    if server.recognition_pipeline.debug:
        print("Recognition diagnostics enabled.", flush=True)
    if server.keep_failed_audio:
        print("Failed audio retention enabled.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping audio receiver")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
