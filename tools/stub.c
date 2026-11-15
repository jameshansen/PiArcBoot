/* The front of PiArcBoot's installer. tools/mkinstaller.py packs the PiArcBoot folder behind it
   in the same file; this unpacks the folder to a temporary one, runs "piarcboot setup" from there
   with this program's own arguments, waits for it and deletes the folder again.

   Behind the program each file is a 14-byte header (the name's length, 2 bytes, its mode, 4, and
   its size, 8, all little-endian), the name with / between folders, and the file. The last 24
   bytes are where the first header starts, 8 bytes, the CRC-32 of everything from there to them,
   4, four zero bytes and "PABSETUP". */
#include <signal.h>
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

#define TAIL 24

static char tmp[1024];
static char **made;             /* what was unpacked, files and folders, in the order they were */
static int nmade;
static uint32_t crc_table[256], crc = 0xFFFFFFFF;

static void clean_up(void)
{
	while (nmade-- > 0)     /* last first, so each folder is empty by the time it comes */
	{
		struct stat st;

		if (!stat(made[nmade], &st) && (st.st_mode & S_IFDIR))
			rmdir(made[nmade]);
		else
			remove(made[nmade]);
	}
	if (tmp[0])
		rmdir(tmp);
}

static void fail(const char *why)
{
	clean_up();
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

/* Reads from the installer and adds what it read to the CRC */
static size_t take(void *buf, size_t n, FILE *f)
{
	size_t got = fread(buf, 1, n, f), i;

	for (i = 0; i < got; i++)
		crc = crc_table[(crc ^ ((unsigned char *)buf)[i]) & 255] ^ (crc >> 8);
	return got;
}

static int run(int argc, char **argv)
{
	char self[1024], path[2048];
	unsigned char head[TAIL];
	uint64_t start, end, size;
	FILE *f, *out;
	uint32_t i, k;
	int status;

	for (i = 0; i < 256; i++)
	{
		uint32_t c = i;

		for (k = 0; k < 8; k++)
			c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
#ifdef _WIN32
	char base[MAX_PATH];

	GetModuleFileNameA(NULL, self, sizeof self);
	GetTempPathA(sizeof base, base);
	snprintf(tmp, sizeof tmp, "%sPiArcBoot-Setup-%lu", base, (unsigned long)GetCurrentProcessId());
	if (make_dir(tmp))
	{
		tmp[0] = 0;
		fail("The installer couldn't make a temporary folder.");
	}
#else
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);

	/* Ctrl+C or a closed terminal stops the installer's pages, which run as a program of their own,
	   and this goes on to tidy up after them */
	signal(SIGINT, SIG_IGN);
	signal(SIGTERM, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	self[n > 0 ? n : 0] = 0;
	snprintf(tmp, sizeof tmp, "%s/piarcboot-setup-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	if (!mkdtemp(tmp))
	{
		tmp[0] = 0;
		fail("The installer couldn't make a temporary folder.");
	}
#endif
	if (!(f = fopen(self, "rb")) || fseek(f, -TAIL, SEEK_END) || fread(head, 1, TAIL, f) != TAIL || memcmp(head + 16, "PABSETUP", 8))
		fail("The installer is damaged. Download it again.");
	end = (uint64_t)ftell(f) - TAIL;
	start = le(head, 8);
	if (start > end)
		fail("The installer is damaged. Download it again.");
	fseek(f, (long)start, SEEK_SET);
	while ((uint64_t)ftell(f) < end)
	{
		unsigned char h[14];
		char name[1024], *p;
		unsigned len = 0;
		static char buf[65536];

		if (take(h, 14, f) != 14 || (len = (unsigned)le(h, 2)) >= sizeof name || take(name, len, f) != len)
			fail("The installer is damaged. Download it again.");
		name[len] = 0;
		size = le(h + 6, 8);
		if (size > end - (uint64_t)ftell(f) || strstr(name, ".."))
			fail("The installer is damaged. Download it again.");
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
			size_t got = take(buf, size < sizeof buf ? (size_t)size : sizeof buf, f);

			if (!got || fwrite(buf, 1, got, out) != got)
			{
				fclose(out);
				fail("The installer couldn't unpack PiArcBoot to a temporary folder.");
			}
			size -= got;
		}
		fclose(out);
#ifndef _WIN32
		chmod(path, (mode_t)le(h + 2, 4) & 0777);
#endif
	}
	fclose(f);
	if ((crc ^ 0xFFFFFFFF) != (uint32_t)le(head + 8, 4))
		fail("The installer is damaged. Download it again.");
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
		int j;

		args[0] = path;
		args[1] = "setup";
		for (j = 1; j < argc; j++)
			args[j + 1] = argv[j];
		if ((pid = fork()) == 0)
		{
			signal(SIGINT, SIG_DFL);
			signal(SIGTERM, SIG_DFL);
			signal(SIGHUP, SIG_DFL);
			signal(SIGQUIT, SIG_DFL);
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
