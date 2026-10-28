import os
import sys
import platform

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))  # => sys/lib
import psutil


class OmsyMonitoring:
	"""Collecte des statistiques système (Linux, macOS, ...) via psutil."""

	def __init__(self, path="/"):
		self.path = path
		self.system = platform.system()
		self._cpu_primed = False

	def get_system_info(self):
		"""Retourne les informations générales du système."""
		return {
			"system": self.system,
			"release": platform.release(),
			"architecture": platform.machine(),
			"logical_cpus": psutil.cpu_count(logical=True),
			"physical_cpus": psutil.cpu_count(logical=False),
			"boot_time": psutil.boot_time(),
		}

	def get_disk_info(self, path=None):
		"""Retourne la capacité et l'espace disque."""
		path = path or self.path
		usage = psutil.disk_usage(path)
		return {
			"path": path,
			"total_bytes": usage.total,
			"used_bytes": usage.used,
			"free_bytes": usage.free,
			"used_percent": round(usage.percent, 2),
		}

	def get_ram_info(self):
		"""Retourne la RAM totale, utilisée et disponible."""
		mem = psutil.virtual_memory()
		available = min(mem.available, mem.total)
		used = max(mem.total - available, 0)
		return {
			"total_bytes": mem.total,
			"used_bytes": used,
			"available_bytes": available,
			"used_percent": round(used / mem.total * 100, 2)
			if mem.total else 0.0,
		}

	def get_swap_info(self):
		"""Retourne les statistiques du swap."""
		swap = psutil.swap_memory()
		return {
			"total_bytes": swap.total,
			"used_bytes": swap.used,
			"free_bytes": swap.free,
			"used_percent": round(swap.percent, 2),
		}

	def get_cpu_info(self, interval=None):
		"""Retourne le nombre de CPU et leur utilisation.

		interval=None : mesure depuis le dernier appel (non bloquant).
		Au tout premier appel, une courte mesure bloquante est faite,
		car psutil n'a pas encore de point de référence.
		"""
		if interval is None and not self._cpu_primed:
			interval = 0.2
		percent = psutil.cpu_percent(interval=interval)
		self._cpu_primed = True
		try:
			load = [round(value, 2) for value in os.getloadavg()]
		except (AttributeError, OSError):
			load = None
		return {
			"logical_cpus": psutil.cpu_count(logical=True),
			"used_percent": round(percent, 2),
			"load_average": load,
		}

	def get_all(self):
		"""Retourne toutes les statistiques disponibles."""
		return {
			"system": self.get_system_info(),
			"disk": self.get_disk_info(),
			"ram": self.get_ram_info(),
			"swap": self.get_swap_info(),
			"cpu": self.get_cpu_info(),
		}