#!/usr/bin/env python3
"""Small standard-library adapters for speech recognition and task structuring.

The module deliberately has no project-specific dependency.  It can be used by
the temporary receiver during development and later by a device-side gateway.
API credentials are read from arguments or environment variables; no secrets
are stored in the repository.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import os
import re
import time
import unicodedata
from typing import Any, Mapping
from urllib.parse import urlencode
from urllib.request import Request, urlopen


class CloudServiceError(RuntimeError):
    """Base error for configuration, transport, or provider failures."""


class CloudConfigurationError(CloudServiceError):
    """Raised when a required provider setting is missing."""


class CloudResponseError(CloudServiceError):
    """Raised when a provider returns malformed or unsuccessful JSON."""


@dataclass(frozen=True)
class BaiduConfig:
    api_key: str = ""
    secret_key: str = ""
    cuid: str = "eink-display"
    dev_pid: int = 1537
    token_url: str = "https://aip.baidubce.com/oauth/2.0/token"
    speech_url: str = "https://vop.baidu.com/server_api"
    timeout: float = 20.0

    @classmethod
    def from_env(cls, environ: Mapping[str, str] | None = None) -> "BaiduConfig":
        env = os.environ if environ is None else environ
        try:
            dev_pid = int(env.get("BAIDU_DEV_PID", "1537"))
            timeout = float(env.get("BAIDU_TIMEOUT", "20"))
        except ValueError as error:
            raise CloudConfigurationError("BAIDU_DEV_PID/BAIDU_TIMEOUT must be numeric") from error
        return cls(
            api_key=env.get("BAIDU_API_KEY", ""),
            secret_key=env.get("BAIDU_SECRET_KEY", ""),
            cuid=env.get("BAIDU_CUID", "eink-display"),
            dev_pid=dev_pid,
            token_url=env.get("BAIDU_TOKEN_URL", cls.token_url),
            speech_url=env.get("BAIDU_SPEECH_URL", cls.speech_url),
            timeout=timeout,
        )


@dataclass(frozen=True)
class DeepSeekConfig:
    api_key: str = ""
    model: str = "deepseek-chat"
    api_url: str = "https://api.deepseek.com/chat/completions"
    timeout: float = 30.0

    @classmethod
    def from_env(cls, environ: Mapping[str, str] | None = None) -> "DeepSeekConfig":
        env = os.environ if environ is None else environ
        try:
            timeout = float(env.get("DEEPSEEK_TIMEOUT", "30"))
        except ValueError as error:
            raise CloudConfigurationError("DEEPSEEK_TIMEOUT must be numeric") from error
        return cls(
            api_key=env.get("DEEPSEEK_API_KEY", ""),
            model=env.get("DEEPSEEK_MODEL", "deepseek-chat"),
            api_url=env.get("DEEPSEEK_API_URL", cls.api_url),
            timeout=timeout,
        )


@dataclass(frozen=True)
class TaskRecord:
    """Normalized task fields in the display order requested by the product."""

    time: str = ""
    place: str = ""
    event: str = ""
    is_task: bool = True
    reason: str = ""


def _require(value: str, name: str) -> str:
    if not value.strip():
        raise CloudConfigurationError(f"{name} is not configured")
    return value.strip()


def _read_json(response: Any) -> Any:
    try:
        raw = response.read()
        if isinstance(raw, bytes):
            raw = raw.decode("utf-8")
        return json.loads(raw)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CloudResponseError(f"invalid provider response: {error}") from error


class BaiduSpeechClient:
    """Baidu OAuth token and raw 16 kHz mono PCM speech client."""

    def __init__(self, config: BaiduConfig, opener=urlopen) -> None:
        self.config = config
        self._opener = opener
        self._access_token: str | None = None
        self._token_expires_at = 0.0

    def get_access_token(self) -> str:
        if self._access_token and time.time() < self._token_expires_at - 60:
            return self._access_token
        key = _require(self.config.api_key, "BAIDU_API_KEY")
        secret = _require(self.config.secret_key, "BAIDU_SECRET_KEY")
        query = urlencode({"grant_type": "client_credentials", "client_id": key, "client_secret": secret})
        request = Request(f"{self.config.token_url}?{query}", method="POST")
        try:
            with self._opener(request, timeout=self.config.timeout) as response:
                payload = _read_json(response)
        except CloudServiceError:
            raise
        except OSError as error:
            raise CloudServiceError(f"Baidu token request failed: {error}") from error
        token = payload.get("access_token") if isinstance(payload, dict) else None
        if not isinstance(token, str) or not token:
            detail = payload.get("error_description", payload) if isinstance(payload, dict) else payload
            raise CloudResponseError(f"Baidu token rejected: {detail}")
        try:
            expires_in = max(0.0, float(payload.get("expires_in", 2592000)))
        except (TypeError, ValueError):
            expires_in = 2592000.0
        self._access_token = token
        self._token_expires_at = time.time() + expires_in
        return token

    def transcribe_pcm(self, pcm: bytes, sample_rate: int = 16000) -> str:
        if not pcm:
            raise CloudServiceError("PCM audio is empty")
        if sample_rate != 16000 or len(pcm) % 2:
            raise CloudServiceError("expected 16 kHz signed PCM16 audio")
        query = urlencode(
            {"cuid": self.config.cuid, "token": self.get_access_token(), "dev_pid": self.config.dev_pid}
        )
        request = Request(
            f"{self.config.speech_url}?{query}",
            data=pcm,
            headers={
                "Content-Type": "audio/pcm;rate=16000",
                "Content-Length": str(len(pcm)),
            },
            method="POST",
        )
        try:
            with self._opener(request, timeout=self.config.timeout) as response:
                payload = _read_json(response)
        except CloudServiceError:
            raise
        except OSError as error:
            raise CloudServiceError(f"Baidu speech request failed: {error}") from error
        if not isinstance(payload, dict) or payload.get("err_no") not in (0, "0"):
            detail = payload.get("err_msg", payload) if isinstance(payload, dict) else payload
            raise CloudResponseError(f"Baidu speech rejected: {detail}")
        results = payload.get("result")
        if not isinstance(results, list) or not results or not isinstance(results[0], str):
            raise CloudResponseError("Baidu speech response has no result")
        return results[0].strip()


DISPLAY_COLUMNS_PER_LINE = 32
TASK_LABEL_COLUMNS = 6
TASK_FIELD_COLUMNS = DISPLAY_COLUMNS_PER_LINE - TASK_LABEL_COLUMNS


def display_columns(text: str) -> int:
    """Return 8-pixel columns used by the embedded 8/16-pixel fonts."""

    columns = 0
    for character in text:
        if unicodedata.combining(character):
            continue
        columns += 2 if unicodedata.east_asian_width(character) in {"F", "W"} else 1
    return columns


def truncate_display_columns(text: str, maximum: int) -> str:
    result: list[str] = []
    columns = 0
    for character in text.replace("\r", " ").replace("\n", " ").strip():
        width = 0 if unicodedata.combining(character) else (
            2 if unicodedata.east_asian_width(character) in {"F", "W"} else 1
        )
        if columns + width > maximum:
            break
        result.append(character)
        columns += width
    return "".join(result).strip()


_SPOKEN_TIME_EQUIVALENTS = {
    "一会": "稍后",
    "一会儿": "稍后",
    "待会": "稍后",
    "待会儿": "稍后",
    "等会": "稍后",
    "等会儿": "稍后",
    "等下": "稍后",
    "等一下": "稍后",
    "今天晚上": "今晚",
    "下班以后": "下班后",
    "下班之后": "下班后",
    "有空": "有空时",
    "抽空": "有空时",
}


def normalize_time_text(text: str) -> str:
    """Convert common spoken relative times without inventing a clock time."""

    value = text.strip()
    return _SPOKEN_TIME_EQUIVALENTS.get(value, value)


def _task_from_mapping(value: Any) -> TaskRecord:
    if not isinstance(value, dict):
        raise CloudResponseError("structured task must be a JSON object")
    aliases = {
        "time": ("time", "时间"),
        "place": ("place", "地点"),
        "event": ("event", "事情", "task"),
    }
    fields: dict[str, str] = {}
    for name, names in aliases.items():
        item = next((value.get(alias) for alias in names if alias in value), "")
        if item is None:
            item = ""
        if not isinstance(item, str):
            raise CloudResponseError(f"task field {name!r} must be a string")
        fields[name] = truncate_display_columns(item, TASK_FIELD_COLUMNS)
    fields["time"] = normalize_time_text(fields["time"])
    raw_is_task = value.get("is_task")
    if raw_is_task is None:
        is_task = any(fields.values())
    elif isinstance(raw_is_task, bool):
        is_task = raw_is_task
    else:
        raise CloudResponseError("task field 'is_task' must be a boolean")
    reason = value.get("reason", "")
    if reason is None:
        reason = ""
    if not isinstance(reason, str):
        raise CloudResponseError("task field 'reason' must be a string")
    if not is_task:
        return TaskRecord(
            is_task=False,
            reason=reason.strip()[:96],
        )
    return TaskRecord(
        **fields,
        is_task=True,
        reason=reason.strip()[:96],
    )


def parse_task_json(content: str) -> TaskRecord:
    """Parse a DeepSeek content string, tolerating a markdown JSON fence."""

    text = content.strip()
    if text.startswith("```"):
        text = re.sub(r"^```(?:json)?\s*|\s*```$", "", text, flags=re.IGNORECASE).strip()
    try:
        value = json.loads(text)
    except json.JSONDecodeError:
        start, end = text.find("{"), text.rfind("}")
        if start < 0 or end <= start:
            raise CloudResponseError("DeepSeek content is not valid JSON")
        try:
            value = json.loads(text[start : end + 1])
        except json.JSONDecodeError as error:
            raise CloudResponseError("DeepSeek content is not valid JSON") from error
    return _task_from_mapping(value)


class DeepSeekClient:
    def __init__(self, config: DeepSeekConfig, opener=urlopen) -> None:
        self.config = config
        self._opener = opener

    def structure_transcript(
        self,
        transcript: str,
        *,
        current_time: str = "",
    ) -> TaskRecord:
        if not transcript.strip():
            raise CloudServiceError("transcript is empty")
        key = _require(self.config.api_key, "DEEPSEEK_API_KEY")
        required_fields = "is_task、time、place、event、reason"
        prompt = (
            "判断下面的中文语音转写是否是一项需要执行、提醒或记录完成状态的待办事项，"
            "并整理为 JSON。第一人称的未来计划、意图或义务，即使采用陈述句表达，"
            "也属于待办；省略主语但包含未来可执行动作的短句也属于待办。已经完成的"
            "过去事实、单纯状态、闲聊、提问或无法执行的内容不属于待办。无法确定但"
            "包含明确可执行动作时，优先按待办处理，避免漏记。只输出 JSON 对象，字段"
            f"必须是 {required_fields}，is_task 必须为布尔值。"
            "缺失字段填空字符串，不得编造信息。把不改变含义的口语改为简洁书面表达，"
            "例如‘一会儿、待会儿、等一下’写为‘稍后’，但不得把相对时间编造成具体"
            "钟点。time、place、event 分别最多13个全角中文字符或26个半角字符，"
            "字段内不得换行。事情字段去掉‘我要、得、记得、一下’等口语框架，保留"
            "动作、对象和必要限定。\n"
            "示例：‘回家要吃饭’=>{\"is_task\":true,\"time\":\"\","
            "\"place\":\"家\",\"event\":\"吃饭\",\"reason\":\"未来行动意图\"}；"
            "‘一会儿要去修手机’=>{\"is_task\":true,\"time\":\"稍后\","
            "\"place\":\"\",\"event\":\"维修手机\",\"reason\":\"未来行动意图\"}；"
            "‘我已经吃过饭了’=>{\"is_task\":false,\"time\":\"\","
            "\"place\":\"\",\"event\":\"\",\"reason\":\"已完成的事实\"}；"
            "‘手机坏了’=>{\"is_task\":false,\"time\":\"\",\"place\":\"\","
            "\"event\":\"\",\"reason\":\"单纯状态\"}。\n转写："
            + transcript
        )
        prompt += f"\n服务器当前时间：{current_time or '未知'}"
        body = json.dumps(
            {
                "model": self.config.model,
                "temperature": 0,
                "messages": [
                    {"role": "system", "content": "你是任务记录整理器。"},
                    {"role": "user", "content": prompt},
                ],
            },
            ensure_ascii=False,
        ).encode("utf-8")
        request = Request(
            self.config.api_url,
            data=body,
            headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"},
            method="POST",
        )
        try:
            with self._opener(request, timeout=self.config.timeout) as response:
                payload = _read_json(response)
        except CloudServiceError:
            raise
        except OSError as error:
            raise CloudServiceError(f"DeepSeek request failed: {error}") from error
        try:
            content = payload["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError) as error:
            raise CloudResponseError("DeepSeek response has no message content") from error
        if not isinstance(content, str):
            raise CloudResponseError("DeepSeek message content is not text")
        return parse_task_json(content)


def format_task(record: TaskRecord, fallback: str = "") -> str:
    """Render fields in the stable Chinese order used by the e-paper UI."""

    if not any((record.time, record.place, record.event)):
        return fallback.strip()
    event = record.event or truncate_display_columns(fallback, TASK_FIELD_COLUMNS)
    values = [
        truncate_display_columns(record.time, TASK_FIELD_COLUMNS),
        truncate_display_columns(record.place, TASK_FIELD_COLUMNS),
        truncate_display_columns(event, TASK_FIELD_COLUMNS),
    ]
    labels = ("时间", "地点", "事情")
    return "\n".join(f"{label}：{value or '-'}" for label, value in zip(labels, values))


__all__ = [
    "BaiduConfig", "BaiduSpeechClient", "CloudConfigurationError",
    "CloudResponseError", "CloudServiceError", "DeepSeekClient", "DeepSeekConfig",
    "DISPLAY_COLUMNS_PER_LINE", "TASK_FIELD_COLUMNS", "TaskRecord",
    "display_columns", "format_task", "normalize_time_text", "parse_task_json",
    "truncate_display_columns",
]
