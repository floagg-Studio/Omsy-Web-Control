STATUS = {
	200: "OK",
	302: "Found",
	400: "Bad Request",
	401: "Unauthorized",
	403: "Forbidden",
	404: "Not Found",
	500: "Internal Server Error"
}

class Response:
	def __init__(self, body=b"", status=200, content_type="text/html; charset=utf-8"):
		if isinstance(body, str):
			body = body.encode("utf-8")
		self.status = int(status)
		self.content_type = content_type
		self.body = body

	def build(self):
		headers = [
			f"HTTP/1.1 {self.status} {STATUS.get(self.status, 'Unknown')}",
			f"Content-Type: {self.content_type}",
			f"Content-Length: {len(self.body)}",
			"Connection: keep-alive",
		]
		return ("\r\n".join(headers) + "\r\n\r\n").encode() + self.body