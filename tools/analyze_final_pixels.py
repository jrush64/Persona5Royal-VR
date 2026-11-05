"""Analyze actual final-frame crops against their recorded rendered camera rotations.

Uses bundled numpy/Pillow only. Does not equate CPU pose labels with image motion.
Lag hypotheses are discriminating only when head angular velocity changes. Animated
objects, lighting, camera translation and postprocessing can also increase residuals.
"""
import argparse
import csv
import json
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image


def load_frames(directory):
    result = []
    with (directory / "frames.csv").open() as stream:
        for row in csv.reader(stream):
            v = np.asarray(row, dtype=float)
            if len(v) not in (64, 67):
                raise ValueError(f"Expected 64 or 67 metadata values, got {len(v)}")
            result.append(dict(id=int(v[0]), serial=int(v[1]), epoch=int(v[3]),
                               full=v[5:7], crop=v[7:11], mismatch=v[13:15],
                               max_byte=v[15:17], quat=v[17:21],
                               fov=v[24:32].reshape(2, 4),
                               view=v[32:64].reshape(2, 4, 4),
                               submitted=(len(v)==64 or (v[64]==1 and v[65]>=0))))
    return sorted(result, key=lambda frame: frame["id"])


def rays(frame, eye, stride=3):
    x, y, width, height = frame["crop"]
    xx, yy = np.meshgrid(np.arange(4, width-4, stride), np.arange(4, height-4, stride))
    lo, hi, up, down = np.tan(frame["fov"][eye])
    rx = lo + (xx+x+.5)/frame["full"][0]*(hi-lo)
    ry = up - (yy+y+.5)/frame["full"][1]*(up-down)
    return xx.astype(int), yy.astype(int), np.stack([rx, ry, -np.ones_like(rx)], -1)


def projected_coords(target, source, eye, image_frame):
    xx, yy, directions = rays(image_frame, eye)
    rotation = source["view"][eye, :3, :3] @ np.linalg.inv(target["view"][eye, :3, :3])
    rotated = directions @ rotation.T
    lo, hi, up, down = np.tan(source["fov"][eye])
    px = (-rotated[..., 0]/rotated[..., 2]-lo)/(hi-lo)*source["full"][0]-.5-image_frame["crop"][0]
    py = (up+rotated[..., 1]/rotated[..., 2])/(up-down)*source["full"][1]-.5-image_frame["crop"][1]
    return xx, yy, px, py


def bilinear(image, x, y):
    h, w = image.shape
    valid = np.isfinite(x) & np.isfinite(y) & (x >= 1) & (x < w-2) & (y >= 1) & (y < h-2)
    x = np.clip(np.nan_to_num(x), 0, w-2)
    y = np.clip(np.nan_to_num(y), 0, h-2)
    ix, iy = x.astype(int), y.astype(int)
    fx, fy = x-ix, y-iy
    value = ((1-fy)*((1-fx)*image[iy, ix]+fx*image[iy, ix+1]) +
             fy*((1-fx)*image[iy+1, ix]+fx*image[iy+1, ix+1]))
    return value, valid


def residual(current_image, previous_image, current, previous, eye, geometry):
    xx, yy, px, py = projected_coords(current, previous, eye, geometry)
    warped, valid = bilinear(previous_image, px, py)
    gy, gx = np.gradient(current_image)
    textured = (np.abs(gx[yy, xx])+np.abs(gy[yy, xx])) > 4
    valid &= textured
    if np.count_nonzero(valid) < 100:
        return None
    difference = current_image[yy, xx]-warped
    difference -= np.median(difference[valid])  # tolerate uniform exposure changes
    return float(np.percentile(np.abs(difference[valid]), 50))


def analyze(directory):
    frames = load_frames(directory)
    images = {}
    for frame in frames:
        for eye, label in enumerate("LR"):
            path = directory / f"{frame['id']:03d}_{label}.ppm"
            if path.exists():
                images[frame["id"], eye] = np.asarray(Image.open(path).convert("L"), dtype=float)
    rows = []
    # Only adjacent captured serials in one app-space epoch and with identical geometry.
    # Larger gaps are excluded instead of pretending they are one-frame transitions.
    for i in range(5, len(frames)):
        f, p = frames[i], frames[i-1]
        if any(frames[i-k]["serial"] != f["serial"]-k for k in range(6)):
            continue
        if not f["submitted"] or not p["submitted"]:
            continue
        if f["serial"] != p["serial"]+1 or f["epoch"] != p["epoch"]:
            continue
        if not np.array_equal(f["crop"], p["crop"]) or not np.array_equal(f["full"], p["full"]):
            continue
        for eye in range(2):
            a, b = images.get((f["id"], eye)), images.get((p["id"], eye))
            if a is None or b is None:
                continue
            scores = {}
            for lag in range(5):
                current, previous = frames[i-lag], frames[i-lag-1]
                if current["epoch"] != f["epoch"] or previous["epoch"] != f["epoch"]:
                    continue
                score = residual(a, b, current, previous, eye, f)
                if score is not None:
                    scores[str(lag)] = score
            frozen = residual(a, b, f, f, eye, f)
            rows.append(dict(id=f["id"], eye="LR"[eye], lag_residual=scores, frozen_residual=frozen))
    medians = {}
    for lag in range(5):
        values = [r["lag_residual"][str(lag)] for r in rows if str(lag) in r["lag_residual"]]
        if values:
            medians[str(lag)] = float(np.median(values))
    center_movements = []
    for a, b in zip(frames[1:], frames[:-1]):
        for eye in range(2):
            def center(f):
                v = f["view"][eye]
                return -np.linalg.solve(v[:3, :3], v[:3, 3])
            center_movements.append(float(np.linalg.norm(center(a)-center(b))))
    mismatches = sum(int(np.sum(f["mismatch"])) for f in frames)
    return dict(frames=len(frames), usable_eye_transitions=len(rows),
                frames_without_successful_submission=sum(not f["submitted"] for f in frames),
                copy_mismatched_pixels=mismatches,
                lag_median_residual=medians,
                best_lag=min(medians, key=medians.get) if medians else None,
                max_camera_translation_game_units=max(center_movements, default=0),
                caveat="Lag ranking is image evidence, not a root-cause verdict. Constant-speed turns cannot identify lag. Moving geometry, translation, UI and postprocessing can confound rotational alignment. Crops exclude the outer lens and runtime display/SSW output.",
                transitions=rows)


def self_test():
    # Render a stationary textured world analytically. A known two-frame-old image
    # paired with current camera labels must be detected from PIXELS, not metadata.
    with tempfile.TemporaryDirectory() as temp:
        directory = Path(temp)
        frames = []
        for i in range(35):
            yaw = .12*np.sin(i*.39)+.035*np.sin(i*.71)
            c, s = np.cos(yaw), np.sin(yaw)
            view = np.eye(4); view[:3, :3] = [[c, 0, -s], [0, 1, 0], [s, 0, c]]
            frames.append(dict(id=i+1, serial=i+120, epoch=1, full=np.array([320, 240]),
                               crop=np.array([0, 0, 320, 240]),
                               view=np.stack([view, view]), fov=np.array([[-.7, .7, .6, -.6]]*2)))
        for injected_lag in (0, 2):
            with (directory / "frames.csv").open("w", newline="") as stream:
                writer = csv.writer(stream)
                for i, frame in enumerate(frames):
                    camera = frames[max(0, i-injected_lag)]
                    _, _, direction = rays(camera, 0, stride=1)
                    # Full image needs all pixels, including margins omitted by rays().
                    xx, yy = np.meshgrid(np.arange(320), np.arange(240))
                    lo, hi, up, down = np.tan(camera["fov"][0])
                    direction = np.stack([lo+(xx+.5)/320*(hi-lo), up-(yy+.5)/240*(up-down), -np.ones_like(xx)], -1)
                    world = direction @ np.linalg.inv(camera["view"][0, :3, :3]).T
                    angle = np.arctan2(world[..., 0], -world[..., 2]); elevation = world[..., 1]/np.linalg.norm(world, axis=-1)
                    pixels = np.clip(128+55*np.sin(angle*91+elevation*43)+48*np.cos(angle*37-elevation*107), 0, 255).astype("uint8")
                    for eye in "LR":
                        Image.fromarray(pixels).convert("RGB").save(directory / f"{frame['id']:03d}_{eye}.ppm")
                    writer.writerow([frame["id"], frame["serial"], 0, 1, i, 320, 240, 0, 0, 320, 240, 0, 0, 0, 0, 0, 0,
                                     0, 0, 0, 1, 0, 0, 0, *frame["fov"].flatten(), *frame["view"].flatten()])
            report = analyze(directory)
            assert report["best_lag"] == str(injected_lag), report["lag_median_residual"]
            print(f"PASS: final-image analysis identifies injected image lag {injected_lag}: {report['lag_median_residual']}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", nargs="?", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    else:
        if args.directory is None:
            parser.error("Provide a captured p5r_finaltrace directory")
        report = analyze(args.directory)
        (args.directory / "analysis.json").write_text(json.dumps(report, indent=2))
        print(json.dumps({k: v for k, v in report.items() if k != "transitions"}, indent=2))
