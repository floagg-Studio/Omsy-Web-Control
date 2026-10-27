import re
import os
import platform
import shutil
import subprocess


class OmsyMonitoring:
	"""Collecte des statistiques système Linux et macOS."""

	def __init__(self, path="/"):
		self.path = path
		self.system = platform.system()
		self._cpu_previous = None

	def get_system_info(self):
		"""Retourne les informations générales du système."""
		return {
			"system": self.system,
			"release": platform.release(),
			"architecture": platform.machine(),
			"logical_cpus": os.cpu_count(),
		}

	def get_disk_info(self, path=None):
		"""Retourne la capacité et l'espace disque."""
		path = path or self.path
		usage = shutil.disk_usage(path)
		return {
			"path": path,
			"total_bytes": usage.total,
			"used_bytes": usage.used,
			"free_bytes": usage.free,
			"used_percent": round(
				usage.used / usage.total * 100, 2
			) if usage.total else 0.0,
		}

	def _read_linux_meminfo(self):
		"""Lit les informations mémoire de Linux."""
		values = {}
		with open("/proc/meminfo", encoding="utf-8") as file:
			for line in file:
				key, raw = line.split(":", 1)
				parts = raw.strip().split()
				if parts:
					values[key] = int(parts[0]) * 1024
		return values

	def _read_macos_vm(self):
		"""Lit les statistiques mémoire de macOS."""
		output = subprocess.check_output(
			["vm_stat"], text=True
		)
		page_size = 4096
		for line in output.splitlines():
			if "page size of" in line:
				page_size = int(
					line.split("page size of")[1].split()[0]
				)
				break
		values = {}
		for line in output.splitlines():
			if ":" not in line:
				continue
			key, raw = line.split(":", 1)
			raw = raw.strip().rstrip(".")
			if raw.isdigit():
				values[key.strip()] = int(raw) * page_size
		return values

	def get_ram_info(self):
		"""Retourne la RAM totale, utilisée et disponible."""
		if self.system == "Linux":
			mem = self._read_linux_meminfo()
			total = mem.get("MemTotal", 0)
			available = mem.get("MemAvailable", mem.get("MemFree", 0))
		elif self.system == "Darwin":
			total = int(subprocess.check_output(
				["sysctl", "-n", "hw.memsize"], text=True
			).strip())
			vm = self._read_macos_vm()
			available = sum(vm.get(key, 0) for key in (
				"Pages free", "Pages inactive", "Pages speculative"
			))
		else:
			raise NotImplementedError("Système non pris en charge")
		available = min(available, total)
		used = max(total - available, 0)
		return {
			"total_bytes": total,
			"used_bytes": used,
			"available_bytes": available,
			"used_percent": round(used / total * 100, 2)
			if total else 0.0,
		}

	
	def get_swap_info(self):
		"""Retourne les statistiques du swap."""
		if self.system == "Linux":
			mem = self._read_linux_meminfo()
			total = mem.get("SwapTotal", 0)
			free = mem.get("SwapFree", 0)
		elif self.system == "Darwin":
			output = subprocess.check_output(
				["sysctl", "-n", "vm.swapusage"], text=True
			)
			values = dict(re.findall(
				r"(total|used|free)\s*=\s*([\d.]+[KMGT]?)",
				output
			))
			total = self._parse_macos_size(values.get("total", "0M"))
			free = self._parse_macos_size(values.get("free", "0M"))
		else:
			raise NotImplementedError("Système non pris en charge")
		used = max(total - free, 0)
		return {
			"total_bytes": total,
			"used_bytes": used,
			"free_bytes": free,
			"used_percent": round(used / total * 100, 2)
			if total else 0.0,
		}


	@staticmethod
	def _parse_macos_size(value):
		"""Convertit une taille macOS en octets."""
		units = {"K": 1024, "M": 1024**2, "G": 1024**3, "T": 1024**4}
		value = value.strip().upper()
		suffix = value[-1]
		number = float(value[:-1]) if suffix in units else float(value)
		return int(number * units.get(suffix, 1))

	def _read_linux_cpu(self):
		"""Lit les compteurs CPU cumulés sous Linux."""
		with open("/proc/stat", encoding="utf-8") as file:
			fields = file.readline().split()[1:]
		values = [int(value) for value in fields]
		values = values[:8]
		idle = values[3] + (values[4] if len(values) > 4 else 0)
		return sum(values), idle

	def _read_macos_cpu(self):
		"""Lit le pourcentage CPU via la commande top."""
		output = subprocess.check_output(
			["top", "-l", "1", "-n", "0"], text=True
		)
		for line in output.splitlines():
			if line.strip().startswith("CPU usage:"):
				parts = line.split(":", 1)[1].split(",")
				values = {}
				for part in parts:
					fields = part.strip().split()
					if len(fields) >= 2 and fields[1] in (
						"user", "sys", "idle"
					):
						values[fields[1]] = float(fields[0].rstrip("%"))
				total = sum(values.values())
				return total, values.get("idle", 0)
		raise RuntimeError("Impossible de lire l'utilisation CPU")

	def get_cpu_info(self):
		"""Retourne le nombre de CPU et leur utilisation."""
		if self.system == "Linux":
			current = self._read_linux_cpu()
		elif self.system == "Darwin":
			current = self._read_macos_cpu()
		else:
			raise NotImplementedError("Système non pris en charge")
		previous = self._cpu_previous
		self._cpu_previous = current
		percent = None
		if previous is not None:
			total_delta = current[0] - previous[0]
			idle_delta = current[1] - previous[1]
			if total_delta > 0:
				percent = round(
					(1 - idle_delta / total_delta) * 100, 2
				)
		return {
			"logical_cpus": os.cpu_count(),
			"used_percent": percent,
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
