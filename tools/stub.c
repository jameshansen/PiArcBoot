/* The front of PiArcBoot's installer. tools/mkinstaller.py packs the PiArcBoot folder behind it
   in the same file; this unpacks the folder to a temporary one, runs "piarcboot setup" from there
   with this program's own arguments, waits for it and deletes the folder again.

   Behind the program each file is a 14-byte header (the name's length, 2 bytes, its mode, 4, and
   its size, 8, all little-endian), the name with / between folders, and the file. The last 16
   bytes are where the first header starts, 8 bytes, and "PABSETUP". */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define make_dir(p) _mkdir(p)
#define EXE "piarcboot.exe"
#else
#include <unistd.h>
#include <sys/wait.h>
#define make_dir(p) mkdir(p, 0755)
#define EXE "piarcboot"
#endif

static char tmp[1024];
static char **made;             /* what was unpacked, files and folders, in the order they were */
static int nmade;

static void fail(const char *why)
{
#ifdef _WIN32
	MessageBoxA(NULL, why, "Install PiArcBoot", MB_ICONERROR);
#else
	fprintf(stderr, "Install PiArcBoot: %s\n", why);
#endif
	exit(1);
}

static void note(const char *path)
{
	made = realloc(made, (nmade + 1) * sizeof *made);
	made[nmade++] = strdup(path);
}

static uint64_t le(const unsigned char *p, int n)
{
	uint64_t v = 0;

	while (n--)
		v = v << 8 | p[n];
	return v;
}

static void clean_up(void)
{
	while (nmade--)         /* last first, so each folder is empty by the time it comes */
	{
		struct stat st;

		if (!stat(made[nmade], &st) && (st.st_mode & S_IFDIR))
			rmdir(made[nmade]);
		else
			remove(made[nmade]);
	}
	rmdir(tmp);
}

static int run(int argc, char **argv)
{
	char self[1024], path[2048];
	unsigned char head[16];
	uint64_t start, end, size;
	FILE *f, *out;
	int status;

#ifdef _WIN32
	char base[MAX_PATH];

	GetModuleFileNameA(NULL, self, sizeof self);
	GetTempPathA(sizeof base, base);
	snprintf(tmp, sizeof tmp, "%sPiArcBoot-Setup-%lu", base, (unsigned long)GetCurrentProcessId());
	if (make_dir(tmp))
		fail("The installer couldn't make a temporary folder.");
#else
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);

	self[n > 0 ? n : 0] = 0;
	snprintf(tmp, sizeof tmp, "%s/piarcboot-setup-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	if (!mkdtemp(tmp))
		fail("The installer couldn't make a temporary folder.");
#endif
	if (!(f = fopen(self, "rb")) || fseek(f, -16, SEEK_END) || fread(head, 1, 16, f) != 16 || memcmp(head + 8, "PABSETUP", 8))
		fail("The installer is damaged. Download it again.");
	end = (uint64_t)ftell(f) - 16;
	start = le(head, 8);
	fseek(f, (long)start, SEEK_SET);
	while ((uint64_t)ftell(f) < end)
	{
		unsigned char h[14];
		char name[1024], *p;
		unsigned len = 0;
		static char buf[65536];

		if (fread(h, 1, 14, f) != 14 || (len = (unsigned)le(h, 2)) >= sizeof name || fread(name, 1, len, f) != len)
			fail("The installer is damaged. Download it again.");
		name[len] = 0;
		size = le(h + 6, 8);
		snprintf(path, sizeof path, "%s/%s", tmp, name);
		for (p = path + strlen(tmp) + 1; *p; p++)
			if (*p == '/')
			{
				*p = 0;
				if (!make_dir(path))
					note(path);
				*p = '/';
			}
		if (!(out = fopen(path, "wb")))
			fail("The installer couldn't unpack PiArcBoot to a temporary folder.");
		note(path);
		while (size)
		{
			size_t got = fread(buf, 1, size < sizeof buf ? (size_t)size : sizeof buf, f);

			if (!got || fwrite(buf, 1, got, out) != got)
				fail("The installer couldn't unpack PiArcBoot to a temporary folder.");
			size -= got;
		}
		fclose(out);
#ifndef _WIN32
		chmod(path, (mode_t)le(h + 2, 4) & 0777);
#endif
	}
	fclose(f);
	snprintf(path, sizeof path, "%s/PiArcBoot/" EXE, tmp);

#ifdef _WIN32
	{
		STARTUPINFOA si = { sizeof si };
		PROCESS_INFORMATION pi;
		char cmd[4096];
		DWORD code = 1;

		(void)argc;
		snprintf(cmd, sizeof cmd, "\"%s\" setup %s", path, argv[0]);
		if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
			fail("The installer couldn't start.");
		WaitForSingleObject(pi.hProcess, INFINITE);
		GetExitCodeProcess(pi.hProcess, &code);
		status = (int)code;
	}
#else
	{
		char **args = calloc(argc + 2, sizeof *args);
		pid_t pid;
		int i;

		args[0] = path;
		args[1] = "setup";
		for (i = 1; i < argc; i++)
			args[i + 1] = argv[i];
		if ((pid = fork()) == 0)
		{
			execv(path, args);
			_exit(127);
		}
		waitpid(pid, &status, 0);
		status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
		if (status == 127)
			fprintf(stderr, "Install PiArcBoot: PiArcBoot couldn't start. It needs SDL2, which you can install with:\n  sudo apt install libsdl2-2.0-0\n");
	}
#endif
	clean_up();
	return status;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
	char *args[1] = { cmdline };

	(void)inst; (void)prev; (void)show;
	return run(1, args);
}
#else
int main(int argc, char **argv)
{
	return run(argc, argv);
}
#endif
