import os

ALLOWED_MIME = {
	"image/png",
	"image/jpeg",
	"text/javascript",
	"text/css",
	"application/json",
	"application/pdf",
	"application/zip",
	"image/svg+xml",
	"video/mp4",
}

ALLOWED_METHOD = {
	"get",
	"post",
}

class Request:
	MAX_BODY = 1 * 1024 * 1024

	method = ""
	path = ""
	lines = {}

	def __init__(self, request_bytes, sock):
		head, _, self.body = request_bytes.partition(b"\r\n\r\n")
		lines = head.split(b"\r\n")
		method, target, _ = lines[0].decode("latin-1").split(" ", 2)
		path, _, query = target.partition("?")
		self.method = method.upper()
		self.path = self._safe_path(path)

		print(f"{self.method} {self.path}")

		self.headers = {}
		for line in lines[1:]:
			if b":" in line:
				k, v = line.split(b":", 1)
				self.headers[k.decode().strip().lower()] = v.decode().strip()

		self._read_body(sock)

	def _safe_path(self, path):
		parts = path.strip("/").split("/")
		if any(p in ("", ".", "..") for p in parts):
			raise ValueError("bad path")
		return "/".join(parts) or "index"

	def _read_body(self, sock):
		try:
			n = int(self.headers.get("content-length", "0"))
		except ValueError:
			raise ValueError("bad content-length")
		if n < 0 or n > self.MAX_BODY:
			raise ValueError("body too large")
		buf = bytearray(self.body)
		while len(buf) < n:
			chunk = sock.recv(min(4096, n - len(buf)))
			if not chunk:
				raise ConnectionError("client closed")
			buf += chunk
		self.body = bytes(buf[:n])