#!/usr/bin/env python3
"""Serve the PS5 music UI and a read-only WAV folder on the local network."""

from __future__ import annotations

import argparse
import json
import socket
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, unquote, urlsplit


CHUNK_SIZE = 256 * 1024
MAX_LIBRARY_TRACKS = 5000
WEB_ROOT = Path(__file__).resolve().parents[2] / "web" / "ps5-music"


class MusicHandler(BaseHTTPRequestHandler):
    server_version = "PS5-SACD-MusicRoom/1.0"
    library_root: Path

    def do_GET(self) -> None:
        request_path = unquote(urlsplit(self.path).path)
        if request_path == "/api/library":
            self._send_library()
        elif request_path.startswith("/music/"):
            self._send_audio(request_path.removeprefix("/music/"), send_body=True)
        else:
            self._send_asset(request_path, send_body=True)

    def do_HEAD(self) -> None:
        request_path = unquote(urlsplit(self.path).path)
        if request_path.startswith("/music/"):
            self._send_audio(request_path.removeprefix("/music/"), send_body=False)
        else:
            self._send_asset(request_path, send_body=False)

    def log_message(self, message: str, *args: object) -> None:
        sys.stdout.write(f"[{self.log_date_time_string()}] {message % args}\n")

    def _send_json(self, data: object) -> None:
        payload = json.dumps(data, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def _send_library(self) -> None:
        records: list[dict[str, object]] = []
        for path in sorted(self.library_root.rglob("*"), key=lambda item: str(item).casefold()):
            if not path.is_file() or path.suffix.casefold() != ".wav":
                continue
            try:
                resolved = path.resolve(strict=True)
                relative = resolved.relative_to(self.library_root)
                stat = resolved.stat()
            except (OSError, ValueError):
                continue

            name = resolved.stem
            artist = resolved.parent.name if resolved.parent != self.library_root else "Your library"
            title = name
            if " - " in name:
                artist_part, title_part = name.split(" - ", 1)
                if artist_part.strip() and title_part.strip():
                    artist, title = artist_part.strip(), title_part.strip()

            relative_name = relative.as_posix()
            records.append({
                "id": relative_name,
                "title": title,
                "artist": artist,
                "album": resolved.parent.name,
                "url": "/music/" + quote(relative_name, safe="/"),
                "sizeBytes": stat.st_size,
            })
            if len(records) >= MAX_LIBRARY_TRACKS:
                break

        self._send_json({"tracks": records, "count": len(records)})

    def _library_file(self, relative_name: str) -> Path | None:
        if not relative_name or "\x00" in relative_name:
            return None
        try:
            candidate = (self.library_root / Path(relative_name)).resolve(strict=True)
            candidate.relative_to(self.library_root)
        except (OSError, ValueError):
            return None
        if not candidate.is_file() or candidate.suffix.casefold() != ".wav":
            return None
        return candidate

    @staticmethod
    def _byte_range(value: str | None, size: int) -> tuple[int, int] | None:
        if value is None:
            return 0, size - 1
        if not value.startswith("bytes=") or "," in value:
            return None
        spec = value[6:].strip()
        if "-" not in spec:
            return None
        first, last = spec.split("-", 1)
        try:
            if first:
                start = int(first)
                end = int(last) if last else size - 1
            else:
                suffix_length = int(last)
                if suffix_length <= 0:
                    return None
                start = max(0, size - suffix_length)
                end = size - 1
        except ValueError:
            return None
        if start < 0 or end < start or start >= size:
            return None
        return start, min(end, size - 1)

    def _send_audio(self, relative_name: str, send_body: bool) -> None:
        path = self._library_file(relative_name)
        if path is None:
            self.send_error(404, "WAV file not found")
            return
        try:
            size = path.stat().st_size
        except OSError:
            self.send_error(404, "WAV file is no longer available")
            return
        if size == 0:
            self.send_error(422, "WAV file is empty")
            return

        selected_range = self._byte_range(self.headers.get("Range"), size)
        if selected_range is None:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        start, end = selected_range
        partial = self.headers.get("Range") is not None
        length = end - start + 1
        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Cache-Control", "no-cache")
        if partial:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()

        if not send_body:
            return
        try:
            with path.open("rb") as source:
                source.seek(start)
                remaining = length
                while remaining:
                    chunk = source.read(min(CHUNK_SIZE, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _send_asset(self, request_path: str, send_body: bool) -> None:
        relative = "index.html" if request_path in ("", "/") else request_path.lstrip("/")
        try:
            path = (WEB_ROOT / relative).resolve(strict=True)
            path.relative_to(WEB_ROOT)
            if not path.is_file():
                raise ValueError("not a file")
            payload_size = path.stat().st_size
        except (OSError, ValueError):
            self.send_error(404, "Page not found")
            return

        content_type = {
            ".html": "text/html; charset=utf-8",
            ".css": "text/css; charset=utf-8",
            ".js": "text/javascript; charset=utf-8",
            ".svg": "image/svg+xml",
        }.get(path.suffix.casefold(), "application/octet-stream")
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(payload_size))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        if send_body:
            try:
                with path.open("rb") as source:
                    while chunk := source.read(CHUNK_SIZE):
                        self.wfile.write(chunk)
            except (BrokenPipeError, ConnectionResetError):
                pass


def local_addresses() -> list[str]:
    addresses: set[str] = set()
    try:
        for result in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            address = result[4][0]
            if not address.startswith(("127.", "169.254.")):
                addresses.add(address)
    except OSError:
        pass
    return sorted(addresses)


def main() -> int:
    parser = argparse.ArgumentParser(description="Serve the PS5-SACD Music Room over your LAN.")
    parser.add_argument("--library", required=True, type=Path, help="folder containing PCM WAV files")
    parser.add_argument("--host", default="0.0.0.0", help="bind address (default: all network interfaces)")
    parser.add_argument("--port", default=8765, type=int, help="HTTP port (default: 8765)")
    args = parser.parse_args()

    library_root = args.library.expanduser().resolve()
    if not library_root.is_dir():
        parser.error(f"WAV library folder does not exist: {library_root}")
    if not WEB_ROOT.is_dir():
        parser.error(f"UI files are missing: {WEB_ROOT}")

    handler_type = type("BoundMusicHandler", (MusicHandler,), {"library_root": library_root})
    server = ThreadingHTTPServer((args.host, args.port), handler_type)
    server.daemon_threads = True
    print(f"Music Room is serving: {library_root}")
    print(f"Open on this PC: http://127.0.0.1:{args.port}/")
    for address in local_addresses():
        print(f"Open from PS5:   http://{address}:{args.port}/")
    print("Only WAV files in the selected folder are visible; the service is read-only.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nMusic Room stopped.")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
