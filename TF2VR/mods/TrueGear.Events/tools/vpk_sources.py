"""Read one reviewed game script without unpacking any archive onto disk.

The optional MIT-licensed tf2-vpk2tar handles Titanfall's LZHAM compression.
Both it and the installed VPK index are pinned before the tool is executed.
"""

from __future__ import annotations

import hashlib
import io
from pathlib import Path
import subprocess
import tarfile
import zlib


EXTRACTOR_SHA256 = "97429c9a0e0791ba818397126d7b05debee5cb2fd081eb5918a97575e3b80220"
MAX_SCRIPT_BYTES = 1024 * 1024
MAX_TAR_BYTES = MAX_SCRIPT_BYTES + 65536
ENTRIES = ("scripts/vscripts/weapons/mp_titanability_basic_block.nut",
           "scripts/vscripts/weapons/mp_titanweapon_vortex_shield.nut")


class VpkSourceError(ValueError):
    """A reviewed script could not be safely read from this installation."""


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_tar_entry(archive: bytes, entry: str) -> bytes:
    """Accept exactly one regular entry, and never write a TAR path to disk."""
    if entry not in ENTRIES:
        raise VpkSourceError("Invalid exact VPK script entry")
    if len(archive) > MAX_TAR_BYTES:
        raise VpkSourceError("Extractor output exceeds the script size limit")
    raw = None
    try:
        with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as stream:
            for member in stream:
                if member.name != entry or not member.isfile() or raw is not None:
                    raise VpkSourceError("Extractor must return only the exact regular script entry")
                if member.size < 1 or member.size > MAX_SCRIPT_BYTES:
                    raise VpkSourceError("VPK script exceeds the script size limit")
                content = stream.extractfile(member)
                if content is None:
                    raise VpkSourceError("Extractor returned an unreadable script")
                raw = content.read(MAX_SCRIPT_BYTES + 1)
                if len(raw) != member.size:
                    raise VpkSourceError("Extractor returned a truncated script")
    except (tarfile.TarError, OSError, EOFError) as error:
        raise VpkSourceError(f"Extractor returned an invalid TAR stream: {error}") from error
    if raw is None:
        raise VpkSourceError(f"Installed VPK is missing the reviewed script: {entry}")
    return raw


def read_reviewed_entry(index: Path, entry: str, extractor: Path, *,
                        expected_index_sha256: str, expected_sha256: str,
                        expected_crc32: str) -> tuple[bytes, dict]:
    """Extract one exact entry and prove its index, content and CRC identity."""
    if entry not in ENTRIES:
        raise VpkSourceError("Invalid exact VPK script entry")
    try:
        if not index.is_file() or _sha256(index.read_bytes()) != expected_index_sha256:
            raise VpkSourceError("Installed VPK index does not match the reviewed game build; no files were changed")
        if (extractor.is_symlink() or not extractor.is_file()
                or _sha256(extractor.read_bytes()) != EXTRACTOR_SHA256):
            raise VpkSourceError("VPK extractor is missing or changed; build the reviewed tool described in tools/vendor/SOURCES.md")
        command = [str(extractor.resolve()), "--threads", "0", "--exclude", "*",
                   "--include", "/" + entry, str(index.resolve())]
        completed = subprocess.run(command, capture_output=True, check=True, timeout=60,
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        raw = read_tar_entry(completed.stdout, entry)
        crc = f"{zlib.crc32(raw) & 0xffffffff:08x}"
        if _sha256(raw) != expected_sha256 or crc != expected_crc32:
            raise VpkSourceError("VPK script SHA256/CRC32 does not match the reviewed game build")
        return raw, {"entry": entry, "entry_crc32": crc,
                     "extractor_sha256": EXTRACTOR_SHA256}
    except subprocess.CalledProcessError as error:
        detail = error.stderr.decode("utf-8", errors="replace").strip()[:800]
        raise VpkSourceError(f"Could not read the installed VPK archives: {detail or error}") from error
    except subprocess.TimeoutExpired as error:
        raise VpkSourceError("Reading the installed VPK timed out; close the game and try again") from error
    except OSError as error:
        raise VpkSourceError(f"Could not read the installed VPK or bundled extractor: {error}") from error
