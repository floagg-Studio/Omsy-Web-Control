import os
import mimetypes

class CGI:
	webinterface = "usr/webinterface/"
	method = ""
	path = ""

	# REPONSE
	REPONSE_STATUS_LINE = "HTTP/1.1 200 OK"
	REPONSE_CONTENT_TYPE = "text/plain"
	REPONSE_HEADERS = {
		"Content-Length": 0,
		"Connection": "keep-alive"
	}

	# ALLOWED_FILE
	ALLOWED_MIME = {
		"image/png",
		"image/jpeg",
		"text/html",
		"text/javascript",
		"text/css",
		"application/json",
		"application/pdf",
		"application/zip",
		"image/svg+xml",
		"video/mp4",
	}

	statusLineCode = {
		200: "OK",
		404: "Not Found",
	}

	def __init__(self, request_bytes, sock):
		headers_part, _, body_part = request_bytes.partition(b"\r\n\r\n")
		lines = headers_part.split(b"\r\n")
		request_line = lines[0].decode(errors="replace")
		self.method, self.path, _ = request_line.split(" ")
		self.path = self.path[1:]
		if(self.path == ''):
			self.path = 'index.html'

		headers_dict = {}
		for line in lines[1:]:
			if b":" in line:
				key, val = line.split(b":", 1)
				key = key.decode().strip().lower()
				val = val.decode().strip()
				headers_dict[key] = val

		content_length = int(headers_dict.get('content-length', 0))
		body = body_part
		while len(body) < content_length:
			body += sock.recv(4096)
		# print(self.templatepath(self.path))

	def cleanPath(self, path):
		clean = path.strip("/")
		if clean == "":
			clean = "index"
		parts = clean.split("/")
		if any(p in ("", ".", "..") for p in parts):
			return None

		return clean

	def templatepath(self, filename):
		return self.webinterface+filename

	def open(self, filename):
		if os.path.isfile(filename) != True:
			return None, None
		
		mime, encoding = mimetypes.guess_type(filename)

		if mime is None:
			return None, None

		if mime not in self.ALLOWED_MIME:
			return None, None

		with open(filename, 'r') as f:
			content = f.read()

		return mime, content

	def response(self, body = ""):
		response = (
			f"{self.REPONSE_STATUS_LINE}"
			f"Content-Type: {self.REPONSE_CONTENT_TYPE}; charset=utf-8\r\n"
			f"Content-Length: {len(body.encode('utf-8'))}\r\n"
			"Connection: keep-alive\r\n"
			"\r\n"
			f"{body}"
		)

		return response.encode('utf-8')

	# SETTER
	def getReponseCode(self):
		return self.REPONSE_STATUS_LINE
	def setReponseCode(self, code):
		self.REPONSE_STATUS_LINE = f"HTTP/1.1 {code} {self.statusLineCode.get(code, '404')}"
	def setContentType(self, mime):
		self.REPONSE_CONTENT_TYPE = mime