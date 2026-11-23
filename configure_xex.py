"""Set the LAN socket privilege in DiscCue 360's generated XEX header."""

import hashlib
import struct
import sys
from pathlib import Path

SYSTEM_FLAGS = 0x00030000
EXECUTION_INFO = 0x00040006
DISCCUE_TITLE_ID = 0x44435545
INSECURE_SOCKETS = 1 << 6


def configure(path):
    image = bytearray(path.read_bytes())
    magic, _, header_size, _, security, count = struct.unpack_from(">4sIIIII", image)
    if magic != b"XEX2" or not 24 + 8 * count <= security <= header_size - 0x184 <= len(image) - 0x184:
        raise ValueError("Unexpected XEX layout")
    entries = {
        key: (offset, value)
        for offset in range(24, 24 + 8 * count, 8)
        for key, value in [struct.unpack_from(">II", image, offset)]
    }
    _, execution = entries[EXECUTION_INFO]
    if execution > header_size - 24 or struct.unpack_from(">I", image, execution + 12)[0] != DISCCUE_TITLE_ID:
        raise ValueError("Expected DiscCue 360's generated executable")
    offset, flags = entries[SYSTEM_FLAGS]
    struct.pack_into(">I", image, offset + 4, flags | INSECURE_SOCKETS)

    # XEX2 header digest covers the header outside the security signature.
    digest = hashlib.sha1(image[security + 0x17C : header_size] + image[: security + 8]).digest()
    image[security + 0x164 : security + 0x178] = digest
    path.write_bytes(image)


if __name__ == "__main__":
    configure(Path(sys.argv[1]))
