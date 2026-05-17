from dataclasses import dataclass
from typing import Any, Mapping

from .protocol import WORKER_HOST, WORKER_PORT


@dataclass
class AudioRuntimeConfig:
    enabled: bool = True
    model_id: str = "microsoft/VibeVoice-Realtime-0.5B"
    device: str = "cuda:0"
    dtype: str = "float16"
    cache_dir: str = "~/.cache/huggingface"
    preload_only: bool = True
    warmup_seconds: float = 0.25
    worker_host: str = WORKER_HOST
    worker_port: int = WORKER_PORT

    @classmethod
    def from_cfg(cls, cfg: Any) -> "AudioRuntimeConfig":
        if cfg is None:
            return cls()

        if isinstance(cfg, Mapping):
            return cls(
                enabled=bool(cfg.get("enabled", True)),
                model_id=str(cfg.get("model_id", cls.model_id)),
                device=str(cfg.get("device", cls.device)),
                dtype=str(cfg.get("dtype", cls.dtype)),
                cache_dir=str(cfg.get("cache_dir", cls.cache_dir)),
                preload_only=bool(cfg.get("preload_only", True)),
                warmup_seconds=float(cfg.get("warmup_seconds", 0.25)),
                worker_host=str(cfg.get("worker_host", WORKER_HOST)),
                worker_port=int(cfg.get("worker_port", WORKER_PORT)),
            )

        return cls(
            enabled=bool(getattr(cfg, "enabled", True)),
            model_id=str(getattr(cfg, "model_id", cls.model_id)),
            device=str(getattr(cfg, "device", cls.device)),
            dtype=str(getattr(cfg, "dtype", cls.dtype)),
            cache_dir=str(getattr(cfg, "cache_dir", cls.cache_dir)),
            preload_only=bool(getattr(cfg, "preload_only", True)),
            warmup_seconds=float(getattr(cfg, "warmup_seconds", 0.25)),
            worker_host=str(getattr(cfg, "worker_host", WORKER_HOST)),
            worker_port=int(getattr(cfg, "worker_port", WORKER_PORT)),
        )
