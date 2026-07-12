#!/usr/bin/env python3
"""Local web app for choosing, per training image, which SR teacher its
target should come from (or excluding the image entirely).

    <python-with-pillow-and-xbrz> tools_training/review_teachers.py \
        [--frames training_data/screenshots] [--port 8642] \
        [--artcnn-onnx PATH]

Then open http://127.0.0.1:8642 — each page shows one image with every
available teacher's x3 output side by side.  Click a teacher (or press its
number key) to record the choice; E excludes the image; arrow keys navigate.

Verdicts are appended to training_data/teacher_manifest.csv as
"relative/path.png,teacher" — the exact format make_targets.py --manifest
consumes ("exclude" rows are skipped there).

Teachers offered: bicubic (baseline), xbrz (rule-based, primary), ArtCNN
(neural-network candidate — x2 ONNX resampled to x3, comparison caveat
applies; auto-enabled when training_data/models/ArtCNN_C4F16.onnx exists
or --artcnn-onnx is given), and original (nearest-neighbor x3, i.e. leave
the pixels alone).  Anime4K is GLSL-only (mpv shader chains) and cannot
run headless, so it is not offered; ArtCNN is the runnable NN stand-in.

Progress is saved to the manifest after EVERY choice, so you can quit and
relaunch any time — the app reopens at your first unreviewed image.
"""

import argparse
import csv
import html
import io
import os
import pathlib
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from PIL import Image

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "native" / "tools" / "distill"))
from teachers import get_teacher  # noqa: E402

SCALE = 3


class State:
    def __init__(self, frames_root, manifest_path, teacher_opts):
        self.root = pathlib.Path(frames_root)
        self.manifest_path = pathlib.Path(manifest_path)
        self.images = sorted(
            str(p.relative_to(self.root))
            for p in self.root.rglob("*.png"))
        if not self.images:
            raise SystemExit(f"no PNGs under {self.root}")
        self.verdicts = {}
        if self.manifest_path.exists():
            with open(self.manifest_path, newline="") as f:
                for row in csv.reader(f):
                    if len(row) >= 2:
                        self.verdicts[row[0]] = row[1]
        self.teacher_opts = teacher_opts
        self.teachers = {}
        names = ["bicubic", "xbrz"]
        if teacher_opts.get("artcnn_onnx"):
            names.append("artcnn")
        names.append("original")   # nearest-neighbor x3: keep the raw pixels
        for n in names:
            self.teachers[n] = get_teacher(n, **teacher_opts)

    def save(self):
        with open(self.manifest_path, "w", newline="") as f:
            w = csv.writer(f)
            for k in sorted(self.verdicts):
                w.writerow([k, self.verdicts[k]])

    def first_unreviewed(self):
        for i, rel in enumerate(self.images):
            if rel not in self.verdicts:
                return i
        return 0


STATE = None

PAGE = """<!doctype html><meta charset="utf-8">
<title>teacher review {idx1}/{total}</title>
<style>
 body {{ background:#181820; color:#ddd; font:14px/1.4 -apple-system,sans-serif;
        margin:16px; }}
 .cards {{ display:flex; flex-wrap:wrap; gap:16px; }}
 .card {{ background:#22222c; padding:10px; border-radius:8px; }}
 .card img {{ image-rendering:pixelated; display:block; max-width:40vw; }}
 .card h3 {{ margin:2px 0 8px; font-size:14px; }}
 a.btn {{ display:inline-block; background:#3355bb; color:#fff; padding:6px 14px;
        border-radius:6px; text-decoration:none; margin:8px 6px 0 0; }}
 a.btn.ex {{ background:#a33; }}
 a.btn.nav {{ background:#444; }}
 .done {{ color:#7c7; }}
</style>
<h2>{name} <small>({idx1}/{total}, {ndone} reviewed)</small>
 {verdict_html}</h2>
<p>
 {choice_buttons}
 <a class="btn ex" href="/verdict?i={idx}&t=exclude">E&nbsp;exclude</a>
 <a class="btn nav" href="/view?i={prev}">&larr; prev</a>
 <a class="btn nav" href="/view?i={next}">next &rarr;</a>
 <a class="btn nav" href="/view?i={next_unrev}">next unreviewed</a>
</p>
<div class="cards">
 {teacher_cards}
</div>
<script>
 const keys = {{ {key_map} , "e": "/verdict?i={idx}&t=exclude" }};
 document.addEventListener("keydown", ev => {{
   if (ev.key === "ArrowLeft")  location = "/view?i={prev}";
   else if (ev.key === "ArrowRight") location = "/view?i={next}";
   else if (keys[ev.key]) location = keys[ev.key];
 }});
</script>
"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _html(self, body, code=200):
        data = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _png(self, img):
        buf = io.BytesIO()
        img.save(buf, "PNG")
        data = buf.getvalue()
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "max-age=3600")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        st = STATE
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)
        idx = max(0, min(len(st.images) - 1, int(q.get("i", ["0"])[0])))
        rel = st.images[idx]

        if url.path == "/":
            self.send_response(302)
            self.send_header("Location", f"/view?i={st.first_unreviewed()}")
            self.end_headers()
            return

        if url.path == "/img":
            t = q.get("t", ["original"])[0]
            if t not in st.teachers:
                return self._html("bad teacher", 404)
            img = Image.open(st.root / rel).convert("RGB")
            return self._png(st.teachers[t](img, SCALE))

        if url.path == "/verdict":
            t = q.get("t", [""])[0]
            if t in st.teachers or t == "exclude":
                st.verdicts[rel] = t
                st.save()
            nxt = idx + 1 if idx + 1 < len(st.images) else st.first_unreviewed()
            self.send_response(302)
            self.send_header("Location", f"/view?i={nxt}")
            self.end_headers()
            return

        if url.path == "/view":
            names = list(st.teachers)
            cards, buttons, keymap = [], [], []
            for j, tn in enumerate(names, start=1):
                cards.append(f'<div class="card"><h3>{j}. {tn}</h3>'
                             f'<img src="/img?i={idx}&t={tn}"></div>')
                buttons.append(f'<a class="btn" href="/verdict?i={idx}&t={tn}">'
                               f'{j}&nbsp;{tn}</a>')
                keymap.append(f'"{j}": "/verdict?i={idx}&t={tn}"')
            v = st.verdicts.get(rel)
            verdict_html = f'<span class="done">[chosen: {html.escape(v)}]</span>' if v else ""
            return self._html(PAGE.format(
                idx=idx, idx1=idx + 1, total=len(st.images),
                ndone=len(st.verdicts), name=html.escape(rel),
                verdict_html=verdict_html,
                choice_buttons=" ".join(buttons),
                teacher_cards="\n".join(cards),
                key_map=", ".join(keymap),
                prev=max(0, idx - 1),
                next=min(len(st.images) - 1, idx + 1),
                next_unrev=st.first_unreviewed()))

        self._html("not found", 404)


def main():
    global STATE
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--frames", default=str(REPO / "training_data" / "screenshots"))
    ap.add_argument("--manifest", default=str(REPO / "training_data" / "teacher_manifest.csv"))
    default_artcnn = REPO / "training_data" / "models" / "ArtCNN_C4F16.onnx"
    ap.add_argument("--artcnn-onnx",
                    default=str(default_artcnn) if default_artcnn.exists() else None,
                    help="ArtCNN ONNX model (NN candidate); auto-detected from "
                         "training_data/models/ if present")
    ap.add_argument("--port", type=int, default=8642)
    args = ap.parse_args()

    STATE = State(args.frames, args.manifest, {"artcnn_onnx": args.artcnn_onnx})
    print(f"[review] {len(STATE.images)} images, {len(STATE.verdicts)} already reviewed")
    print(f"[review] teachers offered: {list(STATE.teachers)}")
    print(f"[review] manifest: {args.manifest}")
    print(f"[review] open http://127.0.0.1:{args.port}")
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
