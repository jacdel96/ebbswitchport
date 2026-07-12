#!/usr/bin/env python3
"""Audit app: shows a random sample of the AUTO-labeled teacher choices
(see auto_label.py) so the human reviewer can agree or overrule each one.

    <python-with-pillow-and-xbrz> tools_training/audit_choices.py \
        [--n 30] [--seed 1] [--port 8643]

Each page shows all candidate outputs at x3 with the auto-chosen one
highlighted.  Press A (agree) or pick the teacher it SHOULD have been
(number keys), or E to say the image should be excluded.  Verdicts go to
training_data/audit_results.csv as "relpath,auto_pick,verdict"; the final
page summarizes agreement and per-game disagreement patterns, which the
annotator then uses to re-label globally.

Only auto-labeled rows are sampled (training_data/auto_labeled.txt), never
the reviewer's own hand-labeled rows.
"""

import argparse
import csv
import html
import io
import pathlib
import random
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from PIL import Image

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "native" / "tools" / "distill"))
from teachers import get_teacher  # noqa: E402

SCALE = 3
CHOICES = ("bicubic", "xbrz", "artcnn", "original")


class State:
    def __init__(self, args):
        self.root = pathlib.Path(args.frames)
        self.results_path = pathlib.Path(args.results)
        manifest = pathlib.Path(args.manifest)
        auto_list = manifest.with_name("auto_labeled.txt")
        if not auto_list.exists():
            raise SystemExit("no auto_labeled.txt — run auto_label.py first")
        labels = {}
        with open(manifest, newline="") as f:
            for row in csv.reader(f):
                if len(row) >= 2:
                    labels[row[0]] = row[1]
        auto = [r for r in auto_list.read_text().splitlines()
                if r.strip() and r in labels and (self.root / r).exists()]
        rng = random.Random(args.seed)
        # stratify across games so 30 samples aren't all from one dir
        by_game = {}
        for r in auto:
            by_game.setdefault(r.split("/", 1)[0], []).append(r)
        sample = []
        games = sorted(by_game)
        gi = 0
        while len(sample) < min(args.n, len(auto)):
            g = games[gi % len(games)]
            gi += 1
            if by_game[g]:
                sample.append(by_game[g].pop(rng.randrange(len(by_game[g]))))
        self.sample = sample
        self.labels = labels
        self.verdicts = {}
        if self.results_path.exists():
            with open(self.results_path, newline="") as f:
                for row in csv.reader(f):
                    if len(row) >= 3:
                        self.verdicts[row[0]] = (row[1], row[2])
        onnx = args.artcnn_onnx
        self.teachers = {n: get_teacher(n, artcnn_onnx=onnx) for n in CHOICES}

    def save(self):
        with open(self.results_path, "w", newline="") as f:
            w = csv.writer(f)
            for k, (pick, verdict) in sorted(self.verdicts.items()):
                w.writerow([k, pick, verdict])

    def next_pending(self):
        for i, rel in enumerate(self.sample):
            if rel not in self.verdicts:
                return i
        return None


STATE = None

PAGE = """<!doctype html><meta charset="utf-8"><title>audit {idx1}/{total}</title>
<style>
 body {{ background:#181820; color:#ddd; font:14px/1.4 -apple-system,sans-serif; margin:16px; }}
 .cards {{ display:flex; flex-wrap:wrap; gap:14px; }}
 .card {{ background:#22222c; padding:8px; border-radius:8px; border:2px solid transparent; }}
 .card.mine {{ border-color:#e6b400; }}
 .card img {{ image-rendering:pixelated; display:block; max-width:44vw; }}
 .card h3 {{ margin:2px 0 6px; font-size:14px; }}
 a.btn {{ display:inline-block; background:#3355bb; color:#fff; padding:6px 14px;
        border-radius:6px; text-decoration:none; margin:8px 6px 0 0; }}
 a.btn.agree {{ background:#2a7; }}
 a.btn.ex {{ background:#a33; }}
 .done {{ color:#7c7; }}
</style>
<h2>{name} <small>({idx1}/{total} audited: {ndone})</small>
 — auto pick: <b style="color:#e6b400">{pick}</b> {verdict_html}</h2>
<p>
 <a class="btn agree" href="/verdict?i={idx}&v=agree">A&nbsp;agree</a>
 {alt_buttons}
 <a class="btn ex" href="/verdict?i={idx}&v=exclude">E&nbsp;exclude image</a>
</p>
<div class="cards">{cards}</div>
<script>
 const keys = {{ "a": "/verdict?i={idx}&v=agree", "e": "/verdict?i={idx}&v=exclude", {key_map} }};
 document.addEventListener("keydown", ev => {{ if (keys[ev.key]) location = keys[ev.key]; }});
</script>
"""

SUMMARY = """<!doctype html><meta charset="utf-8"><title>audit summary</title>
<style> body {{ background:#181820; color:#ddd; font:15px/1.6 -apple-system,sans-serif; margin:24px; }}
 table {{ border-collapse:collapse }} td,th {{ padding:4px 12px; border:1px solid #444 }}</style>
<h2>Audit complete: {agree}/{total} agreed ({pct:.0f}%)</h2>
<h3>Disagreements</h3>
<table><tr><th>image</th><th>auto pick</th><th>your verdict</th></tr>{rows}</table>
<p>Results saved to {path}. The annotator will re-label based on these.</p>
"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, data, ctype):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _redirect(self, loc):
        self.send_response(302)
        self.send_header("Location", loc)
        self.end_headers()

    def do_GET(self):
        st = STATE
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)
        idx = max(0, min(len(st.sample) - 1, int(q.get("i", ["0"])[0])))
        rel = st.sample[idx]

        if url.path == "/":
            n = st.next_pending()
            return self._redirect("/summary" if n is None else f"/view?i={n}")

        if url.path == "/img":
            t = q.get("t", ["original"])[0]
            if t not in st.teachers:
                return self._send(b"bad teacher", "text/plain")
            img = Image.open(st.root / rel).convert("RGB")
            buf = io.BytesIO()
            st.teachers[t](img, SCALE).save(buf, "PNG")
            return self._send(buf.getvalue(), "image/png")

        if url.path == "/verdict":
            v = q.get("v", [""])[0]
            if v in ("agree", "exclude") or v in CHOICES:
                st.verdicts[rel] = (st.labels.get(rel, "?"), v)
                st.save()
            n = st.next_pending()
            return self._redirect("/summary" if n is None else f"/view?i={n}")

        if url.path == "/summary":
            agree = sum(1 for _, v in st.verdicts.values() if v == "agree")
            rows = "".join(
                f"<tr><td>{html.escape(k)}</td><td>{p}</td><td>{v}</td></tr>"
                for k, (p, v) in sorted(st.verdicts.items()) if v != "agree")
            body = SUMMARY.format(agree=agree, total=len(st.verdicts),
                                  pct=100.0 * agree / max(len(st.verdicts), 1),
                                  rows=rows or "<tr><td colspan=3>none</td></tr>",
                                  path=html.escape(str(st.results_path)))
            return self._send(body.encode(), "text/html; charset=utf-8")

        if url.path == "/view":
            pick = st.labels.get(rel, "?")
            cards, alts, keymap = [], [], []
            for j, tn in enumerate(CHOICES, start=1):
                mine = " mine" if tn == pick else ""
                mark = " &#9733; auto pick" if tn == pick else ""
                cards.append(f'<div class="card{mine}"><h3>{j}. {tn}{mark}</h3>'
                             f'<img src="/img?i={idx}&t={tn}"></div>')
                if tn != pick and tn != "original":
                    alts.append(f'<a class="btn" href="/verdict?i={idx}&v={tn}">'
                                f'{j}&nbsp;should be {tn}</a>')
                keymap.append(f'"{j}": "/verdict?i={idx}&v={tn}"')
            got = st.verdicts.get(rel)
            verdict_html = f'<span class="done">[{got[1]}]</span>' if got else ""
            body = PAGE.format(idx=idx, idx1=idx + 1, total=len(st.sample),
                               ndone=len(st.verdicts), name=html.escape(rel),
                               pick=pick, verdict_html=verdict_html,
                               alt_buttons=" ".join(alts), cards="\n".join(cards),
                               key_map=", ".join(keymap))
            return self._send(body.encode(), "text/html; charset=utf-8")

        self._send(b"not found", "text/plain")


def main():
    global STATE
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--frames", default=str(REPO / "training_data" / "screenshots"))
    ap.add_argument("--manifest", default=str(REPO / "training_data" / "teacher_manifest.csv"))
    ap.add_argument("--results", default=str(REPO / "training_data" / "audit_results.csv"))
    default_artcnn = REPO / "training_data" / "models" / "ArtCNN_C4F16.onnx"
    ap.add_argument("--artcnn-onnx",
                    default=str(default_artcnn) if default_artcnn.exists() else None)
    ap.add_argument("--n", type=int, default=30)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--port", type=int, default=8643)
    args = ap.parse_args()

    STATE = State(args)
    print(f"[audit] {len(STATE.sample)} samples, {len(STATE.verdicts)} already audited")
    print(f"[audit] open http://127.0.0.1:{args.port}")
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
