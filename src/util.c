/* Small helpers shared by the whole program: whole files, zip archives, folders and running
   other programs */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "piarcboot.h"

/* A whole file in memory, with a zero byte after it. Free the result. */
uint8_t *file_read(const char *fn, size_t *len)
{
	FILE *f = fopen(fn, "rb");
	uint8_t *d;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = n >= 0 ? malloc(n + 1) : NULL;
	if (d && fread(d, 1, n, f) != (size_t)n)
	{
		free(d);
		d = NULL;
	}
	fclose(f);
	if (d)
	{
		d[n] = 0;
		if (len)
			*len = n;
	}
	return d;
}

int file_write(const char *fn, const void *data, size_t len)
{
	FILE *f = fopen(fn, "wb");
	int ok;

	if (!f)
		return -1;
	ok = fwrite(data, 1, len, f) == len;
	return fclose(f) || !ok ? -1 : 0;
}

const char *base_name(const char *path)
{
	const char *a = strrchr(path, '/'), *b = strrchr(path, '\\');

	if (b > a)
		a = b;
	return a ? a + 1 : path;
}

/* The entries of a zip archive held in memory, from its central directory. Returns how many,
   with *out to be freed. */
int zip_list(const uint8_t *z, size_t len, zipent_t **out)
{
	size_t at, cd;
	int n, i;

	*out = NULL;
	if (len < 22)
		return 0;
	for (at = len - 22; ; at--)     /* the end record, behind which there may be a comment */
	{
		if (!memcmp(z + at, "PK\5\6", 4))
			break;
		if (!at || len - at > 66000)
			return 0;
	}
	n = z[at + 10] | z[at + 11] << 8;
	cd = z[at + 16] | z[at + 17] << 8 | z[at + 18] << 16 | (size_t)z[at + 19] << 24;
	*out = calloc(n ? n : 1, sizeof **out);
	for (i = 0; i < n && cd + 46 <= len && !memcmp(z + cd, "PK\1\2", 4); i++)
	{
		const uint8_t *p = z + cd;
		zipent_t *e = &(*out)[i];
		int name = p[28] | p[29] << 8, extra = p[30] | p[31] << 8, comment = p[32] | p[33] << 8;

		e->method = p[10] | p[11] << 8;
		e->csize = p[20] | p[21] << 8 | p[22] << 16 | (uint32_t)p[23] << 24;
		e->usize = p[24] | p[25] << 8 | p[26] << 16 | (uint32_t)p[27] << 24;
		e->offset = p[42] | p[43] << 8 | p[44] << 16 | (uint32_t)p[45] << 24;
		if (cd + 46 + name > len)
			break;
		snprintf(e->name, sizeof e->name, "%.*s", name, p + 46);
		/* RISC OS's own load and execution addresses and attributes, as SparkFS and Info-ZIP keep
		   them, in an extra field "AC" holding "ARC0" */
		if (cd + 46 + name + extra <= len)
		{
			const uint8_t *x = p + 46 + name, *end = x + extra;

			for (; x + 4 <= end; x += 4 + (x[2] | x[3] << 8))
			{
				int size = x[2] | x[3] << 8;

				/* some zips say the field is longer than it is: what is there is taken */
				if (x[0] == 'A' && x[1] == 'C' && size >= 12 && x + 16 <= end && !memcmp(x + 4, "ARC0", 4))
				{
					e->riscos = 1;
					e->load = x[8] | x[9] << 8 | x[10] << 16 | (uint32_t)x[11] << 24;
					e->exec = x[12] | x[13] << 8 | x[14] << 16 | (uint32_t)x[15] << 24;
					e->attr = size >= 13 && x + 17 <= end ? x[16] : 3;
				}
			}
		}
		cd += 46 + name + extra + comment;
	}
	return i;
}

/* One entry's contents, to be freed, or NULL */
uint8_t *zip_get(const uint8_t *z, size_t len, const zipent_t *e, size_t *n)
{
	const uint8_t *p = z + e->offset;
	uint8_t *out;
	size_t data;
	z_stream s;

	if ((size_t)e->offset + 30 > len || memcmp(p, "PK\3\4", 4))
		return NULL;
	data = e->offset + 30 + (p[26] | p[27] << 8) + (p[28] | p[29] << 8);
	if (data + e->csize > len || e->usize > 64 << 20)
		return NULL;
	out = malloc(e->usize + 1);
	if (e->method == 0)
		memcpy(out, z + data, e->usize < e->csize ? e->usize : e->csize);
	else
	{
		memset(&s, 0, sizeof s);
		inflateInit2(&s, -15);
		s.next_in = (Bytef *)z + data;
		s.avail_in = e->csize;
		s.next_out = out;
		s.avail_out = e->usize;
		if (e->method != 8 || inflate(&s, Z_FINISH) != Z_STREAM_END)
		{
			inflateEnd(&s);
			free(out);
			return NULL;
		}
		inflateEnd(&s);
	}
	*n = e->usize;
	return out;
}

/* A folder and those above it. Returns 0 if it is there afterwards. */
int make_dirs(const char *path)
{
	char buf[1024];
	struct stat st;
	char *p;

	snprintf(buf, sizeof buf, "%s", path);
	for (p = buf + 1; ; p++)
		if (*p == '/' || *p == '\\' || !*p)
		{
			char c = *p;

			*p = 0;
#ifdef _WIN32
			mkdir(buf);
#else
			mkdir(buf, 0755);
#endif
			*p = c;
			if (!c)
				break;
		}
	return stat(path, &st) || !S_ISDIR(st.st_mode) ? -1 : 0;
}

int is_dir(const char *path)
{
	struct stat st;

	return !stat(path, &st) && S_ISDIR(st.st_mode);
}

/* A file, or a folder with everything in it */
void remove_tree(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;
	char buf[1024];

	if (d)
	{
		while ((e = readdir(d)))
			if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			{
				snprintf(buf, sizeof buf, "%s/%s", path, e->d_name);
				remove_tree(buf);
			}
		closedir(d);
		rmdir(path);
	}
	else
		unlink(path);
}

/* Runs a command line and waits for it. Returns its exit status, or -1 if it didn't start.
   On Windows no console window opens for it. */
static int run(char *cmd)
{
#ifdef _WIN32
	STARTUPINFOA si = { sizeof si };
	PROCESS_INFORMATION pi;
	DWORD status = (DWORD)-1;
	char full[4200];

	snprintf(full, sizeof full, "cmd.exe /c \"%s\"", cmd);
	if (!CreateProcessA(NULL, full, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
		return -1;
	WaitForSingleObject(pi.hProcess, INFINITE);
	GetExitCodeProcess(pi.hProcess, &status);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return (int)status;
#else
	int status = system(cmd);

	return status == -1 ? -1 : WEXITSTATUS(status);
#endif
}

int run_command(const char *fmt, ...)
{
	char cmd[4096];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof cmd, fmt, ap);
	va_end(ap);
	return run(cmd);
}

/* Runs a command line and returns what it printed, to be freed, or NULL if it failed */
char *run_output(const char *fmt, ...)
{
	char cmd[4096], *out = NULL;
	size_t n = 0, got;
	va_list ap;
	FILE *f;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof cmd, fmt, ap);
	va_end(ap);
#ifdef _WIN32
	f = _popen(cmd, "r");
#else
	f = popen(cmd, "r");
#endif
	if (!f)
		return NULL;
	do
	{
		out = realloc(out, n + 4097);
		got = fread(out + n, 1, 4096, f);
		n += got;
	} while (got);
	out[n] = 0;
#ifdef _WIN32
	if (_pclose(f))
#else
	if (pclose(f))
#endif
	{
		free(out);
		return NULL;
	}
	return out;
}
