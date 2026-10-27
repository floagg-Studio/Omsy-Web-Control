import os
import sys
import socket

from datetime import datetime

sys.path.insert(0, 'sys/lib/omsy')
try:
	from omsycgi import CGI
	from omsymonitoring import OmsyMonitoring
except ImportError:
	sys.exit(1)

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

def sendResponse(sock, http, mime, response):
	__cgi.setContentType(mime)
	__cgi.setReponseCode(http)
	sock.sendall(response)

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

	__cgi = CGI(request, sock)
	mime, body = __cgi.open(__cgi.templatepath(__cgi.path))
	if(body == None):
		sendResponse(sock, 404, "text/html", "<html><body><h1>Welcome to Omsy Server Python / C</h1></body></html>")
		sock.close()
		break
	if(mime == None):
		sendResponse(sock, 404, "text/html", "<html><body><h1>Welcome to Omsy Server Python / C</h1></body></html>")
		sock.close()
		break

	response = __cgi.response(body)
	log(f"{__cgi.getReponseCode()}/{mime}: {__cgi.templatepath(__cgi.path)}")
	sendResponse(sock, 200, mime, response)

	monitoring = OmsyMonitoring()
	log(monitoring.get_all())

sock.close()
log("Good Bye.")