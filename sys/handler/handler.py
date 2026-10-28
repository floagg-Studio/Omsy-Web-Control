import socket

import sys
sys.path.insert(0, 'sys/lib')

from omsy.response import Response
from omsy.request import Request
from omsy.router import _load_module, load


fd = int(sys.argv[1])
sock = socket.socket(fileno=fd)

while True:
	# 1. lire les en-têtes (cap 64 Ko)
	raw = b""
	while b"\r\n\r\n" not in raw:
		try:
			chunk = sock.recv(4096)
		except (socket.timeout, OSError):
			break
		if not chunk:
			break
		raw += chunk
		if len(raw) > 65536:
			sock.sendall(b"HTTP/1.1 431 Request Header Fields Too Large\r\n\r\n")
			break

	request = Request(raw, sock);
	response = load(request)
	sock.sendall(response.build())

sock.close()