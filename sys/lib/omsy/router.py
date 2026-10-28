import os

import importlib.util

from pathlib import Path

from omsy.response import Response

def _load_module(name):
	ROOT = Path(os.environ.get("OMSY_ROOT", Path.cwd()))
	WEB = ROOT / "usr" / "webinterface"
	path = WEB / f"{name}.py"
	if not path.is_file():
		return

	key = str(path)

	mod_name = f"omsy_page_{name.replace('/', '_')}"
	spec = importlib.util.spec_from_file_location(mod_name, key)
	if spec is None or spec.loader is None:
		return None

	module = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(module)

	return module

def load(request):
	module = _load_module('index')
	if module is None:
		return Response("Not Found", 404)

	view = (
		getattr(module, request.method.lower(), None)
		or getattr(module, "handler", None)
	)
	if view is None:
		return Response("Method Not Allowed", 405)

	return view(request)