"""Pluggable rule-based / NN teachers for super-resolution distillation.

Every teacher is a callable  (PIL.Image RGB, scale:int) -> PIL.Image RGB
whose output is exactly (w*scale, h*scale).  Teachers run on RGB; the
training pipeline converts pairs to luma (BT.601) afterwards.

Teacher policy (per project direction):
  * Rule-based pixel-art scalers (xBRZ, ScaleFX) are the PRIMARY teachers —
    they define the art direction the student distills.
  * NN teachers (ArtCNN, Anime4K) are SECONDARY comparison candidates only:
    ArtCNN ships x2 ONNX models (MIT) so it can run headless, but 2x output
    must be resampled to 3x (quality caveat) and it is biased toward anime
    line art.  Anime4K is GLSL-only — impractical headless, not implemented.
  * The existing romfs espcn_x3.bin checkpoint must NEVER be used as a
    teacher: weights distilled from it would be derivative of that
    unlicensed checkpoint, which is the whole thing we're trying to fix.

This module is intentionally importable on its own (registry pattern) so a
future review app can offer per-image teacher selection and emit a manifest
consumed by make_targets.py --manifest.
"""

from PIL import Image

# name -> (factory, help text).  Factories take the argparse namespace-ish
# options dict and return the teacher callable, so heavyweight deps
# (xbrz, onnxruntime) load only when that teacher is actually selected.
_REGISTRY = {}


def register(name, help_text):
    def deco(factory):
        _REGISTRY[name] = (factory, help_text)
        return factory
    return deco


def teacher_names():
    return sorted(_REGISTRY)


def teacher_help():
    return "; ".join(f"{n}: {h}" for n, (_, h) in sorted(_REGISTRY.items()))


def get_teacher(name, **options):
    """Instantiate a teacher by name. Raises SystemExit with a clear message
    for known-but-unavailable teachers."""
    try:
        factory, _ = _REGISTRY[name]
    except KeyError:
        raise SystemExit(f"unknown teacher {name!r}; available: {teacher_names()}")
    return factory(options)


@register("bicubic", "PIL bicubic resize (baseline, not a real teacher)")
def _bicubic(_options):
    def teach(img, scale):
        return img.resize((img.width * scale, img.height * scale), Image.BICUBIC)
    return teach


@register("original", "nearest-neighbor x3 — keep the raw pixel-art look unchanged")
def _original(_options):
    def teach(img, scale):
        return img.resize((img.width * scale, img.height * scale), Image.NEAREST)
    return teach


@register("xbrz", "xBRZ pixel-art scaler via the xbrz.py ctypes binding (primary)")
def _xbrz(_options):
    try:
        import xbrz
    except ImportError:
        raise SystemExit(
            "the 'xbrz.py' package is not installed. The PyPI sdist is broken "
            "(missing xbrz.h); install from source instead:\n"
            "  git clone https://github.com/ioistired/xbrz.py && pip install ./xbrz.py")
    if not 2 <= 3 <= max(xbrz.SCALE_FACTOR_RANGE):
        raise SystemExit("installed xbrz build does not support x3")

    def teach(img, scale):
        # scale_pillow refuses plain RGB; xBRZ itself is ARGB-native.
        return xbrz.scale_pillow(img.convert("RGBA"), scale).convert("RGB")
    return teach


@register("scalefx", "ScaleFX (GLSL shader) — NOT runnable headless; see notes")
def _scalefx(_options):
    raise SystemExit(
        "scalefx is a GLSL shader (libretro/slang) with no maintained Python "
        "binding; running it means a manual/offline route, e.g. RetroArch with "
        "the scalefx shader + screenshot dumping, or a future GPU harness. "
        "Generate its targets manually into the output dir and skip this flag. "
        "For now use --teacher xbrz (same rule-based family).")


@register("artcnn", "ArtCNN x2 ONNX (MIT) + Lanczos resample to x3 — COMPARISON ONLY")
def _artcnn(options):
    model_path = options.get("artcnn_onnx")
    if not model_path:
        raise SystemExit(
            "--teacher artcnn needs --artcnn-onnx PATH to an ArtCNN ONNX model "
            "(https://github.com/Artoriuz/ArtCNN, MIT — x2, luma-only). "
            "onnxruntime must be installed. NOTE: ArtCNN is x2-only, so output "
            "is Lanczos-resampled 2x->3x (quality caveat), and the models are "
            "trained on anime line art — use for COMPARISON ONLY, not as the "
            "primary teacher.")
    try:
        import onnxruntime as ort
    except ImportError:
        raise SystemExit("--teacher artcnn requires onnxruntime (pip install onnxruntime)")
    import numpy as np

    sess = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
    in_name = sess.get_inputs()[0].name

    def teach(img, scale):
        # ArtCNN operates on luma; run Y through the net (x2), take chroma
        # from a bicubic upscale, then Lanczos-resample the merge to x3.
        w, h = img.size
        ycbcr = img.convert("YCbCr")
        y = np.asarray(ycbcr.split()[0], dtype=np.float32) / 255.0
        out = sess.run(None, {in_name: y[None, None]})[0]
        y2 = np.clip(out[0, 0] * 255.0 + 0.5, 0, 255).astype(np.uint8)
        up2 = img.resize((w * 2, h * 2), Image.BICUBIC).convert("YCbCr")
        _, cb, cr = up2.split()
        merged = Image.merge("YCbCr", (Image.fromarray(y2, "L"), cb, cr)).convert("RGB")
        return merged.resize((w * scale, h * scale), Image.LANCZOS)
    return teach


# Anime4K: GLSL-only (mpv/libplacebo shader chains), no maintained headless
# binding — documented here as impractical for batch target generation.
