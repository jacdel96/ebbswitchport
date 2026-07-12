#!/usr/bin/env python3
"""Distill a small luma-only x3 super-resolution student from teacher-made
targets (see make_targets.py).

    python train_student.py --frames LR_DIR --targets HR_DIR \
        --arch espcn_slim --steps 20000 --out runs/slim1 \
        --export espcn_slim_x3.bin

Architectures:
  espcn_slim  conv 1->32 5x5 +tanh, 32->16 3x3 +tanh, 16->9 3x3,
              PixelShuffle(3).  Same topology/tanh placement as the deployed
              ESPCN, just slimmer — exports to the server's ESP1 format.
  esr_cnn     from-scratch implementation of the eSR-CNN DESIGN from the
              edge-SR paper (arXiv:2108.10335): small conv feature extractor
              (tanh) -> conv head producing 2*C*s^2 channels ->
              PixelShuffle(s) -> split into C value maps + C matching maps,
              output = sum_c V_c * softmax_c(K). ~8K params.  No ESP1 export
              (different topology) — checkpoints are .pt only; server
              integration comes later.

Training: luma-only (BT.601 from RGB), random 64x64 LR crops with aligned
192x192 HR crops, flip/rot90 augmentation, L1 loss, Adam 1e-3 with cosine
decay, batch 64, PSNR-vs-teacher validation on a held-out frame split.
Frames smaller than the crop size are skipped with a warning.
"""

import argparse
import json
import math
import os
import struct
import time

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from PIL import Image

SCALE = 3
CROP = 64
ESP1_MAGIC = b"ESP1"


# ---- models -------------------------------------------------------------


class ESPCNSlim(nn.Module):
    """Deployed ESPCN topology (conv5-tanh-conv3-tanh-conv3-shuffle) with
    slimmer widths.  Keep tanh placement identical to the server's ESPCN."""

    def __init__(self, c1=32, c2=16):
        super().__init__()
        self.conv1 = nn.Conv2d(1, c1, 5, padding=2)
        self.conv2 = nn.Conv2d(c1, c2, 3, padding=1)
        self.conv3 = nn.Conv2d(c2, SCALE * SCALE, 3, padding=1)
        self.shuffle = nn.PixelShuffle(SCALE)
        self.tanh = nn.Tanh()

    def forward(self, x):
        x = self.tanh(self.conv1(x))
        x = self.tanh(self.conv2(x))
        return self.shuffle(self.conv3(x))


class ESRCNN(nn.Module):
    """eSR-CNN-style model, implemented from the edge-SR paper's design
    (arXiv:2108.10335), not from its GPL code.  Feature extractor convs with
    tanh, then a head producing 2*C*s^2 channels; after PixelShuffle the
    first C channels are matching maps K and the last C are value maps V;
    the output is the softmax(K)-weighted sum of V ("template matching")."""

    def __init__(self, c=2, d=8, s=8):
        super().__init__()
        self.c = c
        self.feat = nn.Sequential(
            nn.Conv2d(1, d, 3, padding=1), nn.Tanh(),
            nn.Conv2d(d, s, 3, padding=1), nn.Tanh(),
        )
        self.head = nn.Conv2d(s, 2 * c * SCALE * SCALE, 5, padding=2)
        self.shuffle = nn.PixelShuffle(SCALE)

    def forward(self, x):
        f = self.shuffle(self.head(self.feat(x)))     # (N, 2C, 3H, 3W)
        k, v = f[:, : self.c], f[:, self.c:]
        return (v * torch.softmax(k, dim=1)).sum(dim=1, keepdim=True)


def build_model(arch):
    if arch == "espcn_slim":
        return ESPCNSlim()
    if arch == "esr_cnn":
        return ESRCNN()
    raise SystemExit(f"unknown arch {arch}")


# ---- ESP1 export ----------------------------------------------------------
# Format (matches net_upscale_server.read_weight_arrays; the never-committed
# package_weights.py tool wrote the same layout — verified byte-for-byte
# against the shipped espcn_x3.bin: 4s magic, u32 scale, 3x (4x u32 conv
# weight shape OIHW), then w1,b1,w2,b2,w3,b3 as little-endian f32, no pad):


def export_esp1(model, path):
    if not isinstance(model, ESPCNSlim):
        raise SystemExit("--export (ESP1) only applies to --arch espcn_slim; "
                         "esr_cnn has a different topology — its checkpoints "
                         "stay .pt until the server grows a second backend.")
    convs = [model.conv1, model.conv2, model.conv3]
    with open(path, "wb") as f:
        f.write(ESP1_MAGIC)
        f.write(struct.pack("<I", SCALE))
        for c in convs:
            f.write(struct.pack("<4I", *c.weight.shape))
        for c in convs:
            f.write(np.ascontiguousarray(
                c.weight.detach().cpu().numpy(), dtype="<f4").tobytes())
            f.write(np.ascontiguousarray(
                c.bias.detach().cpu().numpy(), dtype="<f4").tobytes())
    print(f"[export] wrote ESP1 weights -> {path} ({os.path.getsize(path)} bytes)")


# ---- data -----------------------------------------------------------------


def load_luma(path):
    """BT.601 luma in [0,1] float32.  Handles RGB and grayscale PNGs."""
    return load_luma_u8(path).astype(np.float32) / 255.0


def load_luma_u8(path):
    """BT.601 luma as uint8 — the in-RAM dataset format (4x smaller than
    float32; ~2000 frames of float32 LR+HR is ~4.6GB, enough to draw the
    macOS memory killer on a 16GB machine — learned the hard way)."""
    img = Image.open(path)
    if img.mode == "L":
        return np.asarray(img, dtype=np.uint8)
    rgb = np.asarray(img.convert("RGB"), dtype=np.float32)
    y = 0.299 * rgb[..., 0] + 0.587 * rgb[..., 1] + 0.114 * rgb[..., 2]
    return (y + 0.5).astype(np.uint8)


def iter_pngs(d):
    """Relative paths of all PNGs under d (recursive — matches
    make_targets.py, so per-game screenshot subdirs work unchanged)."""
    out = []
    for root, _, files in os.walk(d):
        for f in files:
            if f.lower().endswith(".png"):
                out.append(os.path.relpath(os.path.join(root, f), d))
    return sorted(out)


def load_pairs(frames_dir, targets_dir):
    pairs = []
    for name in iter_pngs(frames_dir):
        tpath = os.path.join(targets_dir, name)
        if not os.path.exists(tpath):
            print(f"[data] skip {name}: no matching target")
            continue
        lr = load_luma_u8(os.path.join(frames_dir, name))
        hr = load_luma_u8(tpath)
        if hr.shape != (lr.shape[0] * SCALE, lr.shape[1] * SCALE):
            print(f"[data] skip {name}: target {hr.shape} is not {SCALE}x of {lr.shape}")
            continue
        if lr.shape[0] < CROP or lr.shape[1] < CROP:
            print(f"[data] skip {name}: smaller than {CROP}x{CROP}")
            continue
        pairs.append((name, lr, hr))
    if not pairs:
        raise SystemExit("no usable frame/target pairs")
    return pairs


def sample_batch(pairs, rng, batch):
    lrs = np.empty((batch, 1, CROP, CROP), dtype=np.float32)
    hrs = np.empty((batch, 1, CROP * SCALE, CROP * SCALE), dtype=np.float32)
    for i in range(batch):
        _, lr, hr = pairs[rng.integers(len(pairs))]
        y = rng.integers(lr.shape[0] - CROP + 1)
        x = rng.integers(lr.shape[1] - CROP + 1)
        a = lr[y:y + CROP, x:x + CROP]
        b = hr[y * SCALE:(y + CROP) * SCALE, x * SCALE:(x + CROP) * SCALE]
        if rng.random() < 0.5:
            a, b = a[:, ::-1], b[:, ::-1]
        if rng.random() < 0.5:
            a, b = a[::-1], b[::-1]
        k = int(rng.integers(4))
        if k:
            a, b = np.rot90(a, k), np.rot90(b, k)
        # dataset is stored uint8; convert per-crop (cheap at 64x64)
        lrs[i, 0] = a.astype(np.float32) / 255.0
        hrs[i, 0] = b.astype(np.float32) / 255.0
    return torch.from_numpy(lrs), torch.from_numpy(hrs)


def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    return 99.0 if mse == 0 else -10.0 * math.log10(mse)


@torch.no_grad()
def validate(model, pairs, device):
    """Full-frame PSNR of the student against the teacher target."""
    model.eval()
    vals = []
    for _, lr, hr in pairs:
        lrf = lr.astype(np.float32) / 255.0
        x = torch.from_numpy(lrf[None, None]).to(device)
        y = model(x).clamp(0, 1)[0, 0].cpu().numpy()
        vals.append(psnr(y, hr.astype(np.float32) / 255.0))
    model.train()
    return float(np.mean(vals))


def bicubic_baseline(pairs):
    """PSNR of plain bicubic x3 vs the teacher target — the bar to beat."""
    vals = []
    for _, lr, hr in pairs:
        im = Image.fromarray(lr, "L")
        up = im.resize((lr.shape[1] * SCALE, lr.shape[0] * SCALE), Image.BICUBIC)
        vals.append(psnr(np.asarray(up, dtype=np.float32) / 255.0,
                         hr.astype(np.float32) / 255.0))
    return float(np.mean(vals))


# ---- training ---------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--frames", help="dir of LR PNGs")
    ap.add_argument("--targets", help="dir of matching 3x target PNGs")
    ap.add_argument("--arch", default="espcn_slim", choices=["espcn_slim", "esr_cnn"])
    ap.add_argument("--steps", type=int, default=20000)
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--val-frac", type=float, default=0.1)
    ap.add_argument("--val-every", type=int, default=250)
    ap.add_argument("--out", default="runs/default", help="checkpoint dir")
    ap.add_argument("--resume", help="checkpoint (.pt) to load before training/export")
    ap.add_argument("--export", help="write ESP1 binary here after training "
                                     "(espcn_slim only; use --steps 0 to only export)")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--device", default="mps" if torch.backends.mps.is_available() else "cpu")
    args = ap.parse_args()

    device = torch.device(args.device)
    torch.manual_seed(args.seed)
    model = build_model(args.arch).to(device)
    n_params = sum(p.numel() for p in model.parameters())
    print(f"[model] {args.arch}: {n_params} params, device={device}")

    if args.resume:
        ck = torch.load(args.resume, map_location="cpu")
        if ck.get("arch", args.arch) != args.arch:
            raise SystemExit(f"checkpoint arch {ck.get('arch')} != --arch {args.arch}")
        model.load_state_dict(ck["model"])
        print(f"[model] resumed from {args.resume} (step {ck.get('step')})")

    if args.steps > 0:
        if not (args.frames and args.targets):
            raise SystemExit("--frames and --targets are required unless --steps 0")
        pairs = load_pairs(args.frames, args.targets)
        rng = np.random.default_rng(args.seed)
        order = rng.permutation(len(pairs))
        n_val = max(1, int(len(pairs) * args.val_frac))
        val_pairs = [pairs[i] for i in order[:n_val]]
        train_pairs = [pairs[i] for i in order[n_val:]]
        print(f"[data] {len(train_pairs)} train / {len(val_pairs)} val frames")
        base = bicubic_baseline(val_pairs)
        print(f"[data] bicubic-vs-teacher baseline on val: {base:.2f} dB")

        opt = torch.optim.Adam(model.parameters(), lr=args.lr)
        sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=args.steps)
        os.makedirs(args.out, exist_ok=True)
        best = -1.0
        model.train()
        t0 = time.perf_counter()
        loss_acc, loss_n = 0.0, 0
        for step in range(1, args.steps + 1):
            x, y = sample_batch(train_pairs, rng, args.batch)
            x, y = x.to(device), y.to(device)
            opt.zero_grad(set_to_none=True)
            loss = F.l1_loss(model(x), y)
            loss.backward()
            opt.step()
            sched.step()
            loss_acc += loss.item()
            loss_n += 1
            if step % args.val_every == 0 or step == args.steps:
                v = validate(model, val_pairs, device)
                rate = step / (time.perf_counter() - t0)
                print(f"step {step:6d}  L1 {loss_acc / loss_n:.5f}  "
                      f"val PSNR {v:.2f} dB (bicubic {base:.2f})  "
                      f"lr {sched.get_last_lr()[0]:.2e}  {rate:.1f} steps/s")
                loss_acc, loss_n = 0.0, 0
                state = {"arch": args.arch, "step": step, "model": model.state_dict(),
                         "opt": opt.state_dict(), "val_psnr": v,
                         "args": {k: v2 for k, v2 in vars(args).items()}}
                torch.save(state, os.path.join(args.out, "last.pt"))
                if v > best:
                    best = v
                    torch.save(state, os.path.join(args.out, "best.pt"))
        with open(os.path.join(args.out, "summary.json"), "w") as f:
            json.dump({"arch": args.arch, "params": n_params, "steps": args.steps,
                       "best_val_psnr": best, "bicubic_baseline": base}, f, indent=2)
        print(f"[train] done. best val PSNR {best:.2f} dB "
              f"(+{best - base:.2f} over bicubic) -> {args.out}")
        # export the best weights, not whatever the last step left behind
        if args.export:
            ck = torch.load(os.path.join(args.out, "best.pt"), map_location="cpu")
            model.load_state_dict(ck["model"])

    if args.export:
        export_esp1(model.cpu(), args.export)
    elif args.arch == "esr_cnn":
        print("[note] esr_cnn: no ESP1 export (different topology); "
              "use the .pt checkpoints in --out")


if __name__ == "__main__":
    main()
