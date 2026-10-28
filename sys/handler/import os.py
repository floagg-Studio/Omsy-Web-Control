import os
import sys
import socket

from datetime import datetime

sys.path.insert(0, 'sys/lib')
from omsy._omsycgi import OmsyCGI
from omsy.omsymonitoring import OmsyMonitoring

fd = int(sys.argv[1])
sock = socket.socket(fileno=fd)
# DEBUG
from pathlib import Path
dossier_actuel = Path.cwd()
# print(dossier_actuel)
# DEBUGs

def log(message, *args):
	print(
		f"{datetime.now().strftime('%Y-%m-%d %H:%M:%S')} "
		f"([{os.getpid()}/{os.getppid()}/{fd}] Python) {message}",
		*args,
		file=sys.stderr
	)

log(f"Welcome.")
while True:
	request = b""
	try:
		while b"\r\n\r\n" not in request:
			chunk = sock.recv(4096)
			if not chunk:
				break
			request += chunk
	except (socket.timeout, BlockingIOError, OSError) as e:
		log(f"TIMEOUT ({e})")
		break
	if not request:
		log("CONNEXION FERMEE par le client")
		break

	__cgi = OmsyCGI(request, sock)
	log(__cgi.request_method, __cgi.request_path)
	__cgi.send(sock)

	monitoring = OmsyMonitoring()
	log(monitoring.get_all())

sock.close()
log("Good Bye.")