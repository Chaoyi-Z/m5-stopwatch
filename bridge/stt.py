"""
Transcription using local Whisper (faster-whisper).

No API key required. First run downloads the model (~74 MB for "base").
Much more accurate than the Google Web Speech API, especially for
technical / coding vocabulary and background-noise environments.

Fallback: if faster-whisper is not installed, reverts to Google STT.
Install:  pip install faster-whisper
"""
import os
import numpy as np

# Pre-load CUDA 12 DLLs into the process so ctranslate2's C++ runtime finds them.
# ctranslate2 uses LoadLibrary at runtime — ctypes.CDLL pre-loads them first.
def _preload_cuda_dlls():
    import ctypes, glob, site
    search_roots = site.getsitepackages() + [
        os.path.join(os.environ.get("APPDATA", ""), "Python", "Python313", "site-packages"),
    ]
    targets = [
        os.path.join("nvidia", "cublas", "bin", "cublas64_12.dll"),
        os.path.join("nvidia", "cublas", "bin", "cublasLt64_12.dll"),
        os.path.join("nvidia", "cudnn",  "bin", "cudnn64_9.dll"),
        os.path.join("nvidia", "cudnn",  "bin", "cudnn_ops64_9.dll"),
    ]
    for root in search_roots:
        for t in targets:
            p = os.path.join(root, t)
            if os.path.isfile(p):
                try:
                    ctypes.CDLL(p)
                except Exception:
                    pass

_preload_cuda_dlls()

_model = None


def _get_model():
    global _model
    if _model is None:
        from faster_whisper import WhisperModel
        print("[STT] Loading Whisper 'large-v3' on GPU (first run ~1.5 GB)…")
        _model = WhisperModel("large-v3", device="cuda", compute_type="float16")
        print("[STT] Whisper large-v3 ready on GPU.")
    return _model


def transcribe_pcm(data: bytes, sample_rate: int = 16000) -> str:
    """
    Transcribe raw PCM audio (int16 mono, little-endian).
    Returns recognised text, or "" if nothing was understood.
    """
    try:
        model = _get_model()

        # Convert int16 → float32 in [-1, 1] as Whisper expects
        audio = np.frombuffer(data, dtype=np.int16).astype(np.float32) / 32768.0

        segments, info = model.transcribe(
            audio,
            language="en",
            beam_size=10,
            initial_prompt=(
                "Technical English speech from a software engineer with a Chinese accent. "
                "Topics: code, Claude AI, Python, C++, debugging, functions, variables, "
                "terminal, Git, VS Code, machine learning, API, server, database."
            ),
            condition_on_previous_text=False,  # avoid hallucination loops
            vad_filter=True,
            vad_parameters=dict(
                min_silence_duration_ms=300,
                speech_pad_ms=200,
            ),
        )
        text = " ".join(seg.text.strip() for seg in segments).strip()

        if text:
            print(f"[STT] Recognised ({info.language} {info.language_probability:.0%}): {text!r}")
        else:
            print("[STT] No speech detected (try speaking closer to the watch).")
        return text

    except ImportError:
        return _fallback_google(data, sample_rate)


def _fallback_google(data: bytes, sample_rate: int) -> str:
    """Google Web Speech API fallback when faster-whisper is not installed."""
    print("[STT] faster-whisper not found — using Google STT (run: pip install faster-whisper)")
    import speech_recognition as sr
    r = sr.Recognizer()
    audio = sr.AudioData(data, sample_rate, 2)
    try:
        result = r.recognize_google(audio)
        print(f"[STT] Google recognised: {result!r}")
        return result
    except sr.UnknownValueError:
        print("[STT] No speech recognised.")
        return ""
    except sr.RequestError as e:
        print(f"[STT] Google API error: {e}")
        return ""
