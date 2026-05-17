from dataclasses import dataclass
import os
from typing import Any, Optional


@dataclass
class LoadedAudioModel:
    model_id: str
    device: str
    dtype: str
    model: Any
    processor: Any
    snapshot_path: Optional[str]


def _resolve_torch_dtype(dtype_name: str):
    import torch

    dt = (dtype_name or "").lower()
    if dt in ("fp16", "float16", "half"):
        return torch.float16
    if dt in ("bf16", "bfloat16"):
        return torch.bfloat16
    if dt in ("fp32", "float32"):
        return torch.float32
    return torch.float16


def _try_snapshot_download(model_id: str, cache_dir: str) -> Optional[str]:
    try:
        from huggingface_hub import snapshot_download

        return snapshot_download(
            repo_id=model_id,
            cache_dir=os.path.expanduser(cache_dir),
            resume_download=True,
        )
    except Exception:
        return None


def _load_processor(load_from: str, cache_dir: str):
    """Load processor/feature-extractor from local snapshot path or model id."""
    try:
        from transformers import AutoProcessor

        return AutoProcessor.from_pretrained(
            load_from,
            cache_dir=os.path.expanduser(cache_dir),
            trust_remote_code=True,
        )
    except Exception:
        return None


def _load_model(load_from: str, cache_dir: str, torch_dtype):
    """Load model using the local snapshot path when available.

    Loading from the local path lets transformers resolve the custom modeling
    files (e.g. modeling_vibevoice_streaming.py) from disk instead of
    re-fetching them, which also registers the custom architecture class.
    AutoConfig is loaded first with trust_remote_code=True so that the
    custom class is registered before the model weights are mapped.
    """
    from transformers import AutoConfig, AutoModel

    expanded_cache = os.path.expanduser(cache_dir)
    common_kwargs = dict(trust_remote_code=True)

    # Step 1: load config with trust_remote_code so custom arch is registered.
    config = AutoConfig.from_pretrained(
        load_from,
        cache_dir=expanded_cache,
        **common_kwargs,
    )

    # Step 2: load model from the same local path using the resolved config.
    return AutoModel.from_pretrained(
        load_from,
        config=config,
        cache_dir=expanded_cache,
        torch_dtype=torch_dtype,
        low_cpu_mem_usage=True,
        **common_kwargs,
    )


def load_vibevoice_model(model_id: str, device: str, dtype: str, cache_dir: str) -> LoadedAudioModel:
    import torch

    torch_dtype = _resolve_torch_dtype(dtype)

    # Download/verify snapshot first; load_from becomes the local directory so
    # transformers can resolve custom modeling code from the cached files.
    snapshot_path = _try_snapshot_download(model_id, cache_dir)
    load_from = snapshot_path if snapshot_path else model_id

    processor = _load_processor(load_from, cache_dir)
    model = _load_model(load_from, cache_dir, torch_dtype)
    model = model.eval()

    if device:
        model = model.to(device)

    return LoadedAudioModel(
        model_id=model_id,
        device=device,
        dtype=dtype,
        model=model,
        processor=processor,
        snapshot_path=snapshot_path,
    )


def warmup_loaded_model(bundle: LoadedAudioModel) -> bool:
    import torch

    model = bundle.model
    if model is None:
        return False

    # Best-effort warmup with minimal assumptions about model signature.
    with torch.inference_mode():
        try:
            input_ids = torch.zeros((1, 2), dtype=torch.long, device=bundle.device)
            model(input_ids=input_ids)
            return True
        except Exception:
            pass

        try:
            model.generate(max_new_tokens=1)
            return True
        except Exception:
            pass

    return False
