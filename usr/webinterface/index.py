from omsy.response import Response


def rendering(path):
	with open("usr/webinterface/src/index.html", 'r') as f:
		contenu = f.read()

	return contenu


def get(request):
	contenu = rendering("usr/webinterface/src/index.html")
	return Response(contenu)