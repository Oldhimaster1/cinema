"""Prove experimental 5bpp packing is lossless, distinct from valid CIN2 v2."""
import random
import sys
from pathlib import Path

import pytest
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import packed5_lab as lab  # noqa: E402


@pytest.mark.parametrize("pattern", ["zero", "max", "alternating", "ramp", "random"])
def test_packed5_exact_roundtrip(pattern):
    gen = random.Random(451)
    data = {
        "zero": lambda i: 0,
        "max": lambda i: 31,
        "alternating": lambda i: 31 if i % 2 else 0,
        "ramp": lambda i: i % 32,
        "random": lambda i: gen.randrange(32),
    }
    indices = bytes(data[pattern](i) for i in range(lab.FRAME_PIXELS))
    packed = lab.pack5(indices)
    assert len(packed) == 9600
    assert lab.unpack5(packed) == indices
    assert packed == lab.pack5(indices)


def test_packed5_rejects_malformed_input():
    with pytest.raises(ValueError):
        lab.pack5([0] * 8)
    with pytest.raises(ValueError):
        lab.pack5([0] * (lab.FRAME_PIXELS - 1) + [32])
    with pytest.raises(ValueError):
        lab.unpack5(b"\x00" * 9599)


def test_32_color_metrics_are_reproducible_and_real_lcd_precision():
    colors = [(i * 7 % 256, i * 11 % 256, i * 19 % 256) for i in range(64)]
    source = Image.new("RGB", (160, 96))
    source.putdata([colors[(x // 8 + y // 6) % len(colors)]
                    for y in range(96) for x in range(160)])
    result = lab.compare([source, source.copy()])
    assert result == lab.compare([source, source.copy()])
    assert result["pixels"] == 160 * 96 * 2
    assert result["packed5_sector_bytes"] == 9728
    assert result["packed5_kib_per_second_24fps"] == 228.0
    assert result["packed5_kib_per_second_30fps"] == 285.0
    assert result["weighted_mse_32"] >= 0
