"""
Audio worker — standalone TCP server that runs in .venv (Python 3.10+).

Loaded by launch_closd_pilotlight.sh as a background process.
CLoSD (Python 3.8) connects via the protocol module and sends text prompts;
this process owns the VibeVoice model and generates speech.

Usage (handled by the launch script automatically):
    .venv/bin/python -m audio_runtime.worker [options]
"""

import argparse
import json
import os
import socket
import sys
import threading
import time
import wave
from typing import Any, Optional

# ---------------------------------------------------------------------------
# Resolve project root so imports work when run as __main__
# ---------------------------------------------------------------------------
_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from audio_runtime.protocol import WORKER_HOST, WORKER_PORT, send_msg  # noqa: E402


# ---------------------------------------------------------------------------
# Model loading
# ---------------------------------------------------------------------------

def _load_vibevoice(model_id: str, device: str, cache_dir: str):
    """Load VibeVoice processor and model from local snapshot or Hub."""
    from huggingface_hub import snapshot_download
    from vibevoice.modular.modeling_vibevoice_streaming_inference import (
        VibeVoiceStreamingForConditionalGenerationInference,
    )
    from vibevoice.processor.vibevoice_streaming_processor import (
        VibeVoiceStreamingProcessor,
    )

    cache_dir = os.path.expanduser(cache_dir)

    # Ensure snapshot is on disk (fast if already cached)
    try:
        local_path = snapshot_download(repo_id=model_id, cache_dir=cache_dir)
    except Exception as ex:
        print("[audio-worker] snapshot_download failed, trying model_id directly: %s" % ex)
        local_path = model_id

    print("[audio-worker] loading processor from %s" % local_path)
    processor = VibeVoiceStreamingProcessor.from_pretrained(
        local_path, trust_remote_code=True
    )

    import torch

    load_dtype = torch.bfloat16 if device.startswith("cuda") else torch.float32
    attn_impl = "flash_attention_2" if device.startswith("cuda") else "sdpa"

    print("[audio-worker] loading model (dtype=%s attn=%s) ..." % (load_dtype, attn_impl))
    try:
        model = VibeVoiceStreamingForConditionalGenerationInference.from_pretrained(
            local_path,
            torch_dtype=load_dtype,
            device_map=device if device.startswith("cuda") else None,
            attn_implementation=attn_impl,
        )
    except Exception:
        print("[audio-worker] flash_attention_2 failed, falling back to sdpa")
        model = VibeVoiceStreamingForConditionalGenerationInference.from_pretrained(
            local_path,
            torch_dtype=load_dtype,
            device_map=device if device.startswith("cuda") else None,
            attn_implementation="sdpa",
        )

    model.eval()
    model.set_ddpm_inference_steps(num_steps=5)
    return processor, model


# ---------------------------------------------------------------------------
# Synthesis helper
# ---------------------------------------------------------------------------

def _synthesize(text: str, processor, model, output_dir: str, device: str) -> Optional[str]:
    """Run VibeVoice on `text`, save a .wav, return the path (or None on error)."""
    import torch
    import copy

    try:
        inputs = processor(text=text, return_tensors="pt", padding=True)
        for k, v in inputs.items():
            if torch.is_tensor(v):
                inputs[k] = v.to(device)

        with torch.inference_mode():
            outputs = model.generate(
                **inputs,
                max_new_tokens=None,
                cfg_scale=1.5,
                tokenizer=processor.tokenizer,
                generation_config={"do_sample": False},
                verbose=False,
                all_prefilled_outputs=None,
            )

        if not (outputs.speech_outputs and outputs.speech_outputs[0] is not None):
            return None

        os.makedirs(output_dir, exist_ok=True)
        ts = int(time.time() * 1000)
        out_path = os.path.join(output_dir, "audio_%d.wav" % ts)
        processor.save_audio(outputs.speech_outputs[0], output_path=out_path)
        return out_path

    except Exception as ex:
        print("[audio-worker] synthesis error: %s" % ex)
        return None


# ---------------------------------------------------------------------------
# TCP server
# ---------------------------------------------------------------------------

class AudioWorkerServer:
    def __init__(self, host: str, port: int, processor: Any, model: Any,
                 output_dir: str, device: str) -> None:
        self.host = host
        self.port = port
        self.processor = processor
        self.model = model
        self.output_dir = output_dir
        self.device = device
        self._synth_lock = threading.Lock()  # one synthesis at a time

    def _handle_client(self, conn: socket.socket, addr) -> None:
        print("[audio-worker] client connected: %s" % str(addr))
        try:
            fobj = conn.makefile("r", encoding="utf-8")
            for raw in fobj:
                raw = raw.strip()
                if not raw:
                    continue
                try:
                    msg = json.loads(raw)
                except json.JSONDecodeError:
                    send_msg(conn, {"status": "error", "msg": "invalid json"})
                    continue

                cmd = msg.get("cmd", "")

                if cmd == "ping":
                    send_msg(conn, {"status": "pong", "ready": True})

                elif cmd == "synthesize":
                    text = msg.get("text", "").strip()
                    if not text:
                        send_msg(conn, {"status": "error", "msg": "empty text"})
                        continue
                    # Run synthesis in a thread so the socket stays responsive
                    def _run(t=text):
                        with self._synth_lock:
                            path = _synthesize(
                                t, self.processor, self.model,
                                self.output_dir, self.device,
                            )
                        if path:
                            print("[audio-worker] saved: %s" % path)
                        else:
                            print("[audio-worker] synthesis produced no output")
                    threading.Thread(target=_run, daemon=True).start()
                    send_msg(conn, {"status": "ok"})

                elif cmd == "shutdown":
                    send_msg(conn, {"status": "ok"})
                    break

                else:
                    send_msg(conn, {"status": "error", "msg": "unknown cmd: %s" % cmd})
        except Exception as ex:
            print("[audio-worker] client error: %s" % ex)
        finally:
            conn.close()
            print("[audio-worker] client disconnected: %s" % str(addr))

    def run(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind((self.host, self.port))
            srv.listen(4)
            print("[audio-worker] ready on %s:%d" % (self.host, self.port))
            while True:
                conn, addr = srv.accept()
                t = threading.Thread(
                    target=self._handle_client, args=(conn, addr), daemon=True
                )
                t.start()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="VibeVoice audio worker")
    p.add_argument("--host", default=WORKER_HOST)
    p.add_argument("--port", type=int, default=WORKER_PORT)
    p.add_argument("--model", default="microsoft/VibeVoice-Realtime-0.5B")
    p.add_argument("--device", default="cuda:0")
    p.add_argument("--cache-dir", default="~/.cache/huggingface")
    p.add_argument("--output-dir", default="/tmp/mixed-motion-audio")
    return p.parse_args()


if __name__ == "__main__":
    args = _parse_args()

    print("[audio-worker] starting — model=%s device=%s" % (args.model, args.device))
    try:
        processor, model = _load_vibevoice(args.model, args.device, args.cache_dir)
    except Exception as ex:
        print("[audio-worker] FATAL: could not load model: %s" % ex)
        sys.exit(1)

    print("[audio-worker] model loaded")
    server = AudioWorkerServer(
        host=args.host,
        port=args.port,
        processor=processor,
        model=model,
        output_dir=args.output_dir,
        device=args.device,
    )
    server.run()
