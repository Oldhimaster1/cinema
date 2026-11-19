import pytest

from tools.check_ce_memory import verify_memory


def fake_map(size: int, bss_end: int = 0xD13000):
    return (
        " 0x00d1a87f LOAD_ADDR = 0xd1a87f\n"
        " 0x00d13fd8 BSSHEAP_HIGH = 0xd13fd8\n"
        f" 0x{0xD1A87F + size:08x} ___runprgm_reloc_end = .\n"
        f" 0x{bss_end:08x} ___bss_high = .\n"
    )


def test_safely_below_64_kib():
    assert verify_memory(fake_map(60000)) == (60000, 5536)


def test_boundary_64_kib():
    assert verify_memory(fake_map(65536)) == (65536, 0)


@pytest.mark.parametrize("size", [65537, 110000, 130512, 0, -1])
def test_reject_oversized_and_invalid(size):
    with pytest.raises(ValueError):
        verify_memory(fake_map(size))


def test_reject_bss_overlap():
    with pytest.raises(ValueError, match="BSS"):
        verify_memory(fake_map(60000, 0xD14000))


def test_reject_missing_symbols():
    with pytest.raises(ValueError, match="missing"):
        verify_memory("not a CEdev map")
