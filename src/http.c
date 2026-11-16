/* Downloads over HTTP and HTTPS, inside the program: through WinHTTP on Windows, and elsewhere
   through libcurl's shared library, loaded the first time a download is wanted, as the menu
   bar loads GTK. Redirects are followed, an error status is a failure, a connection gets 20
   seconds to open, and a download as long as the caller allows. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "piarcboot.h"

#define MOST (512u << 20)       /* the most a download may be */

typedef struct
{
	uint8_t *data;
	size_t len, cap;
} grow_t;

static int grow_add(grow_t *g, const void *p, size_t n)
{
	if (g->len + n > MOST)
		return -1;
	if (g->len + n + 1 > g->cap)
	{
		size_t cap = g->cap ? g->cap : 65536;
		uint8_t *d;

		while (cap < g->len + n + 1)
			cap *= 2;
		if (!(d = realloc(g->data, cap)))
			return -1;
		g->data = d;
		g->cap = cap;
	}
	memcpy(g->data + g->len, p, n);
	g->len += n;
	g->data[g->len] = 0;    /* kept ending in a 0, so a page can be read as text */
	return 0;
}

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

static int get(const char *url, int seconds, grow_t *g)
{
	wchar_t wurl[2048], host[256], path[2048];
	URL_COMPONENTS u;
	HINTERNET session = NULL, conn = NULL, req = NULL;
	DWORD status = 0, size = sizeof status, avail, got, start = GetTickCount();
	int ok = 0;
	char buf[65536];

	if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, sizeof wurl / sizeof *wurl))
		return -1;
	memset(&u, 0, sizeof u);
	u.dwStructSize = sizeof u;
	u.lpszHostName = host;
	u.dwHostNameLength = sizeof host / sizeof *host;
	u.lpszUrlPath = path;
	u.dwUrlPathLength = sizeof path / sizeof *path;
	u.dwExtraInfoLength = 1;        /* the query stays on the path */
	if (!WinHttpCrackUrl(wurl, 0, 0, &u))
		return -1;
	session = WinHttpOpen(L"PiArcBoot", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (session)
	{
		WinHttpSetTimeouts(session, 20000, 20000, 30000, 60000);
		conn = WinHttpConnect(session, host, u.nPort, 0);
	}
	if (conn)
		req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
					 u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
	if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
	    WinHttpReceiveResponse(req, NULL) &&
	    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
				WINHTTP_NO_HEADER_INDEX) &&
	    status < 400)
	{
		ok = 1;
		while (ok && WinHttpQueryDataAvailable(req, &avail) && avail)
		{
			if (GetTickCount() - start > (DWORD)seconds * 1000 ||
			    !WinHttpReadData(req, buf, avail < sizeof buf ? avail : sizeof buf, &got) || grow_add(g, buf, got))
				ok = 0;
		}
	}
	if (req)
		WinHttpCloseHandle(req);
	if (conn)
		WinHttpCloseHandle(conn);
	if (session)
		WinHttpCloseHandle(session);
	return ok ? 0 : -1;
}
#else
#include <dlfcn.h>

/* The few parts of libcurl's interface used, by their fixed numbers in curl.h */
#define CURLOPT_WRITEDATA 10001
#define CURLOPT_URL 10002
#define CURLOPT_TIMEOUT 13
#define CURLOPT_USERAGENT 10018
#define CURLOPT_WRITEFUNCTION 20011
#define CURLOPT_FAILONERROR 45
#define CURLOPT_FOLLOWLOCATION 52
#define CURLOPT_MAXREDIRS 68
#define CURLOPT_CONNECTTIMEOUT 78
#define CURLOPT_NOSIGNAL 99
#define CURL_GLOBAL_DEFAULT 3

static struct
{
	int (*global_init)(long);
	void *(*easy_init)(void);
	int (*easy_setopt)(void *, int, ...);
	int (*easy_perform)(void *);
	void (*easy_cleanup)(void *);
} curl;

static size_t write_part(char *p, size_t size, size_t n, void *g)
{
	return grow_add(g, p, size * n) ? 0 : size * n;
}

static int get(const char *url, int seconds, grow_t *g)
{
	static int loaded;      /* 1 loaded, -1 not to be had */
	void *c;
	int r;

	if (!loaded)
	{
		static const char *const libs[] = { "libcurl.so.4", "libcurl-gnutls.so.4" };
		static const char *const names[] = { "curl_global_init", "curl_easy_init", "curl_easy_setopt", "curl_easy_perform", "curl_easy_cleanup" };
		void **slot = (void **)&curl, *lib = NULL;
		size_t i;

		for (i = 0; !lib && i < sizeof libs / sizeof *libs; i++)
			lib = dlopen(libs[i], RTLD_NOW);
		loaded = lib ? 1 : -1;
		for (i = 0; lib && i < sizeof names / sizeof *names; i++)
			if (!(slot[i] = dlsym(lib, names[i])))
				loaded = -1;
		if (loaded > 0)
			curl.global_init(CURL_GLOBAL_DEFAULT);
		else
			fprintf(stderr, "piarcboot: downloads need libcurl (libcurl4), which isn't installed%c", 10);
	}
	if (loaded < 0 || !(c = curl.easy_init()))
		return -1;
	curl.easy_setopt(c, CURLOPT_URL, url);
	curl.easy_setopt(c, CURLOPT_USERAGENT, "PiArcBoot");
	curl.easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl.easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
	curl.easy_setopt(c, CURLOPT_FAILONERROR, 1L);
	curl.easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
	curl.easy_setopt(c, CURLOPT_TIMEOUT, (long)seconds);
	curl.easy_setopt(c, CURLOPT_NOSIGNAL, 1L);     /* downloads run on threads of their own */
	curl.easy_setopt(c, CURLOPT_WRITEFUNCTION, write_part);
	curl.easy_setopt(c, CURLOPT_WRITEDATA, (void *)g);
	r = curl.easy_perform(c);
	curl.easy_cleanup(c);
	return r ? -1 : 0;
}
#endif

/* A page or file, to be freed, with its length in *len if len isn't NULL; NULL if it didn't
   arrive within that many seconds. It ends in a 0 past its length, so it can be read as text. */
uint8_t *http_get(const char *url, int seconds, size_t *len)
{
	grow_t g = { 0 };

	if (get(url, seconds, &g) || !g.data)
	{
		free(g.data);
		return NULL;
	}
	if (len)
		*len = g.len;
	return g.data;
}
