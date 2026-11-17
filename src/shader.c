/* RetroArch's GLSL shader presets (.glslp), run on the picture by the graphics chip. Each of a
   preset's passes draws into a texture of its own, at the size its scale names, and the last draws
   at the size the picture is shown.

   The passes run in the SDL renderer's own OpenGL or OpenGL ES context, so the game window asks
   for one of those before it opens. The renderer keeps its own record of what it last set in the
   context, so whatever the passes change is put back after them.

   Taken from the format: shaderN, aliasN, filter_linearN, srgb_framebufferN, scale_typeN and
   scale_type_xN/yN (source, viewport, absolute), scaleN and scale_xN/yN, wrap_modeN and
   the textures a preset loads from PNG files. The passes see the uniforms RetroArch gives
   them: Texture, Orig, PassN, PassPrevN and an alias, each with its TextureSize and InputSize,
   OutputSize, FrameCount, FrameDirection and MVPMatrix. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <SDL_opengl.h>
#include "piarcboot.h"

#define MAX_PASSES 16
#define MAX_LUTS 8
#define MAX_BINDS 16
#define MAX_ATTRIBS 8
#define MAX_PARAMS 128

#define GLFUNCS \
	F(void, GetIntegerv, (GLenum, GLint *)) \
	F(GLboolean, IsEnabled, (GLenum)) \
	F(void, Enable, (GLenum)) \
	F(void, Disable, (GLenum)) \
	F(const GLubyte *, GetString, (GLenum)) \
	F(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
	F(void, GenTextures, (GLsizei, GLuint *)) \
	F(void, DeleteTextures, (GLsizei, const GLuint *)) \
	F(void, BindTexture, (GLenum, GLuint)) \
	F(void, ActiveTexture, (GLenum)) \
	F(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
	F(void, TexParameteri, (GLenum, GLenum, GLint)) \
	F(void, GenerateMipmap, (GLenum)) \
	F(void, PixelStorei, (GLenum, GLint)) \
	F(void, DrawArrays, (GLenum, GLint, GLsizei)) \
	F(void, GenFramebuffers, (GLsizei, GLuint *)) \
	F(void, DeleteFramebuffers, (GLsizei, const GLuint *)) \
	F(void, BindFramebuffer, (GLenum, GLuint)) \
	F(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
	F(GLuint, CreateShader, (GLenum)) \
	F(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
	F(void, CompileShader, (GLuint)) \
	F(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
	F(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
	F(void, DeleteShader, (GLuint)) \
	F(GLuint, CreateProgram, (void)) \
	F(void, AttachShader, (GLuint, GLuint)) \
	F(void, BindAttribLocation, (GLuint, GLuint, const GLchar *)) \
	F(void, LinkProgram, (GLuint)) \
	F(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
	F(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
	F(void, DeleteProgram, (GLuint)) \
	F(void, UseProgram, (GLuint)) \
	F(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
	F(void, Uniform1i, (GLint, GLint)) \
	F(void, Uniform1f, (GLint, GLfloat)) \
	F(void, Uniform2f, (GLint, GLfloat, GLfloat)) \
	F(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
	F(void, BindBuffer, (GLenum, GLuint)) \
	F(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
	F(void, EnableVertexAttribArray, (GLuint)) \
	F(void, DisableVertexAttribArray, (GLuint)) \
	F(void, GetVertexAttribiv, (GLuint, GLenum, GLint *)) \
	F(void, Clear, (GLbitfield))

static struct
{
#define F(ret, name, args) ret (APIENTRY *name) args;
	GLFUNCS
#undef F
} gl;

enum { BY_SOURCE, BY_VIEWPORT, BY_PIXELS };
enum { ORIG = -1, LUT = 100 };          /* what a pass reads besides passes before it: the picture, or a preset's texture */

typedef struct
{
	GLint tex, tex_size, in_size;   /* its sampler, and the uniforms with its size */
	int from;                       /* ORIG, an earlier pass, or LUT + which */
} bind_t;

typedef struct
{
	GLuint prog;
	int type[2];
	float scale[2];
	int linear, repeat, srgb;       /* how it reads what is before it, and an sRGB texture for what it draws */
	char alias[64];
	GLint mvp, frame_count, frame_dir, out_size;
	int nbinds;
	bind_t bind[MAX_BINDS];         /* the textures it reads, one to a texture unit */
	GLuint tex;                     /* what it draws, but for the last pass */
	int w, h;
	int still, fresh, resized;      /* it reads nothing that changes from frame to frame; it was drawn this frame; at a new size */
} pass_t;

static pass_t pass[MAX_PASSES];
static int npasses, es, glsl, frame;    /* es: OpenGL ES's version or 0; glsl: the shading language's, 120 for 1.20 */
static GLuint luts[MAX_LUTS], fbo;
static char lut_id[MAX_LUTS][64];       /* the names the passes know them by */
static int nluts;
static SDL_Texture *in_tex, *out_tex;  /* the picture as the passes read it, and the last one's drawing */
static int in_w, in_h, out_w, out_h;
static int still_w, still_h;            /* the picture's size when the passes last ran */
static char param_name[MAX_PARAMS][48]; /* the settings the passes have as uniforms, from their #pragma parameter lines */
static float param_val[MAX_PARAMS];
static int nparams;

/* ---- The preset ---- */

static char (*keys)[64], (*vals)[512];
static int nkeys;

/* A preset's line, key = "value" with the quotes left off at will and a # comment after it */
static void preset_line(char *s)
{
	char *eq = strchr(s, '='), *k = s, *v, *e;

	if (!eq || *s == '#')
		return;
	*eq = 0;
	while (*k == ' ' || *k == '\t')
		k++;
	for (e = eq; e > k && (e[-1] == ' ' || e[-1] == '\t'); e--)
		e[-1] = 0;
	for (v = eq + 1; *v == ' ' || *v == '\t'; v++)
		;
	if (*v == '"')
	{
		v++;
		v[strcspn(v, "\"")] = 0;
	}
	else
		v[strcspn(v, " \t#\r")] = 0;
	keys = realloc(keys, (nkeys + 1) * sizeof *keys);
	vals = realloc(vals, (nkeys + 1) * sizeof *vals);
	snprintf(keys[nkeys], sizeof *keys, "%s", k);
	snprintf(vals[nkeys], sizeof *vals, "%s", v);
	nkeys++;
}

/* The value of a key, named with printf's format and a pass's number, or NULL */
static const char *opt(const char *fmt, int i)
{
	char key[64];
	int k;

	snprintf(key, sizeof key, fmt, i);
	for (k = nkeys - 1; k >= 0; k--)
		if (!strcmp(keys[k], key))
			return vals[k];
	return NULL;
}

static int opt_true(const char *fmt, int i)
{
	const char *v = opt(fmt, i);

	return v && (!strcmp(v, "true") || !strcmp(v, "1"));
}

static int scale_type(const char *v)
{
	return !v ? -1 : !strcmp(v, "viewport") ? BY_VIEWPORT : !strcmp(v, "absolute") ? BY_PIXELS : BY_SOURCE;
}

/* ---- GL ---- */

static void set_sampling(GLuint tex, int linear, int repeat, int mipmap)
{
	gl.BindTexture(GL_TEXTURE_2D, tex);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmap ? (linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST) : linear ? GL_LINEAR : GL_NEAREST);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
}

/* Which of the bound texture's channels a read of it gives as red, blue and alpha */
static void swizzle(GLint r, GLint b, GLint a)
{
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, r);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, b);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, a);
}

/* One stage of a pass from its file: the same text for both, told which it is by VERTEX or
   FRAGMENT. Its #version stays on desktop OpenGL; OpenGL ES has 300 es for 130 and up, as
   RetroArch gives it. */
static GLuint compile(GLenum type, const char *text, const char *file)
{
	const char *body = text, *v = strstr(text, "#version"), *src[3];
	char version[512], log[2048];
	GLuint s;
	GLint ok;
	int n = 110;

	if (v)
	{
		n = atoi(v + 8);
		body = strchr(v, '\n') ? strchr(v, '\n') + 1 : "";
	}
	if (es)
		snprintf(version, sizeof version, n >= 130 ? "#version 300 es\n" : "#version 100\n");
	else if (n > glsl && glsl >= 110)
		/* A Mac's OpenGL 2.1, which goes to GLSL 1.20: the calls the shaders make from 1.30 are made
		   from 1.20's own. textureLod reads at the level the hardware picks, which for these
		   shaders' pictures, never shrunk, is the same one. */
		snprintf(version, sizeof version, "#version %d\n#define round(x) floor((x) + 0.5)\n"
			 "#define trunc(x) (sign(x) * floor(abs(x)))\n#define tanh(x) (1.0 - 2.0 / (exp(2.0 * clamp(x, -10.0, 10.0)) + 1.0))\n"
			 "#define texture(s, c) texture2D(s, c)\n#define textureLod(s, c, l) texture2D(s, c)\n", glsl);
	else
		snprintf(version, sizeof version, "#version %d\n", n);
	src[0] = version;
	src[1] = type == GL_VERTEX_SHADER ? "#define VERTEX\n#define PARAMETER_UNIFORM\n" : "#define FRAGMENT\n#define PARAMETER_UNIFORM\n";
	src[2] = body;
	s = gl.CreateShader(type);
	gl.ShaderSource(s, 3, src, NULL);
	gl.CompileShader(s);
	gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		gl.GetShaderInfoLog(s, sizeof log, NULL, log);
		fprintf(stderr, "piarcboot: %s didn't compile: %s\n", file, log);
		gl.DeleteShader(s);
		return 0;
	}
	return s;
}

/* A pass's settings, #pragma parameter name "what it is" default min max step, which the preset
   may give values of its own */
static void read_params(const char *text)
{
	const char *at = text, *v;
	char name[48], what[128];
	float value;
	int k;

	while ((at = strstr(at, "#pragma parameter ")) && nparams < MAX_PARAMS)
	{
		at += 18;
		if (sscanf(at, "%47s \"%127[^\"]\" %f", name, what, &value) != 3)
			continue;
		for (k = 0; k < nparams && strcmp(param_name[k], name); k++)
			;
		snprintf(param_name[k], sizeof *param_name, "%s", name);
		param_val[k] = (v = opt(name, 0)) ? (float)atof(v) : value;
		nparams += k == nparams;
	}
}

/* A pass's text with its settings as constants: each "uniform ... float name;" line of a setting
   becomes a #define of its value. The graphics chip's compiler then drops the branches and the
   sums a setting decides, which it can't with a uniform, and a Raspberry Pi 400 draws CRT-Royale
   a quarter faster for it. A line never grows to twice its length. */
static char *fold_params(char *text)
{
	char *out = malloc(strlen(text) * 2 + 1), *o = out, *line = text, *end;
	char name[48];
	int k, n;

	if (!out)
		return text;
	for (; *line; line = end)
	{
		const char *at = line;

		end = strchr(line, '\n');
		end = end ? end + 1 : line + strlen(line);
		while (*at == ' ' || *at == '\t')
			at++;
		n = 0;
		if (!strncmp(at, "uniform ", 8) && (at = strstr(at, " float ")) && at < end && sscanf(at + 7, " %47[A-Za-z0-9_] %n", name, &n) == 1 && at[7 + n] == ';')
		{
			for (k = 0; k < nparams && strcmp(param_name[k], name); k++)
				;
			if (k < nparams)
			{
				o += sprintf(o, "#define %s float(%.9g)\n", name, param_val[k]);
				continue;
			}
		}
		memcpy(o, line, end - line);
		o += end - line;
	}
	*o = 0;
	free(text);
	return out;
}

static GLuint program(const char *file)
{
	char *text = (char *)file_read(file, NULL), log[2048];
	GLuint vs, fs, p = 0;
	GLint ok;

	if (!text)
	{
		fprintf(stderr, "piarcboot: %s can't be read\n", file);
		return 0;
	}
	text = fold_params(text);
	vs = compile(GL_VERTEX_SHADER, text, file);
	fs = vs ? compile(GL_FRAGMENT_SHADER, text, file) : 0;
	free(text);
	if (fs)
	{
		p = gl.CreateProgram();
		gl.AttachShader(p, vs);
		gl.AttachShader(p, fs);
		gl.BindAttribLocation(p, 0, "VertexCoord");    /* where the renderer has its own position, colour and coordinates */
		gl.BindAttribLocation(p, 1, "COLOR");
		gl.BindAttribLocation(p, 2, "TexCoord");
		gl.LinkProgram(p);
		gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
		if (!ok)
		{
			gl.GetProgramInfoLog(p, sizeof log, NULL, log);
			fprintf(stderr, "piarcboot: %s didn't link: %s\n", file, log);
			gl.DeleteProgram(p);
			p = 0;
		}
	}
	if (vs)
		gl.DeleteShader(vs);
	if (fs)
		gl.DeleteShader(fs);
	return p;
}

/* A texture a pass reads, if its program has uniforms for it by that name */
static void add_bind(pass_t *p, const char *base, int from)
{
	char name[96];
	bind_t *b = &p->bind[p->nbinds];

	if (p->nbinds >= MAX_BINDS)
		return;
	snprintf(name, sizeof name, "%sTexture", base);
	b->tex = gl.GetUniformLocation(p->prog, name);
	snprintf(name, sizeof name, "%sTextureSize", base);
	b->tex_size = gl.GetUniformLocation(p->prog, name);
	snprintf(name, sizeof name, "%sInputSize", base);
	b->in_size = gl.GetUniformLocation(p->prog, name);
	b->from = from;
	if (b->tex >= 0 || b->tex_size >= 0 || b->in_size >= 0)
		p->nbinds++;
}

/* The context's state that the passes change, kept to be put back */
typedef struct
{
	GLint prog, fbo, viewport[4], active, tex0, buffer, attrib[MAX_ATTRIBS];
	GLboolean blend, scissor, srgb;
} saved_t;

static void save_state(saved_t *s)
{
	int i;

	gl.GetIntegerv(GL_CURRENT_PROGRAM, &s->prog);
	gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &s->fbo);
	gl.GetIntegerv(GL_VIEWPORT, s->viewport);
	gl.GetIntegerv(GL_ACTIVE_TEXTURE, &s->active);
	gl.ActiveTexture(GL_TEXTURE0);
	gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex0);
	gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->buffer);
	for (i = 0; i < MAX_ATTRIBS; i++)
		gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &s->attrib[i]);
	s->blend = gl.IsEnabled(GL_BLEND);
	s->scissor = gl.IsEnabled(GL_SCISSOR_TEST);
	s->srgb = !es && gl.IsEnabled(GL_FRAMEBUFFER_SRGB);
	gl.Disable(GL_BLEND);
	gl.Disable(GL_SCISSOR_TEST);
}

static void restore_state(const saved_t *s, int units)
{
	int i;

	for (i = units - 1; i >= 0; i--)
	{
		gl.ActiveTexture(GL_TEXTURE0 + i);
		gl.BindTexture(GL_TEXTURE_2D, i ? 0 : s->tex0);
	}
	gl.ActiveTexture(s->active);
	gl.BindFramebuffer(GL_FRAMEBUFFER, s->fbo);
	gl.Viewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
	gl.UseProgram(s->prog);
	gl.BindBuffer(GL_ARRAY_BUFFER, s->buffer);
	for (i = 0; i < MAX_ATTRIBS; i++)
		if (s->attrib[i])
			gl.EnableVertexAttribArray(i);
		else
			gl.DisableVertexAttribArray(i);
	if (s->blend)
		gl.Enable(GL_BLEND);
	if (s->scissor)
		gl.Enable(GL_SCISSOR_TEST);
	if (!es)
		(s->srgb ? gl.Enable : gl.Disable)(GL_FRAMEBUFFER_SRGB);
}

/* ---- Loading ---- */

void shader_close(void)
{
	int i;

	for (i = 0; i < npasses; i++)
	{
		if (pass[i].prog)
			gl.DeleteProgram(pass[i].prog);
		if (pass[i].tex)
			gl.DeleteTextures(1, &pass[i].tex);
	}
	if (nluts)
		gl.DeleteTextures(nluts, luts);
	if (fbo)
		gl.DeleteFramebuffers(1, &fbo);
	if (in_tex)
		SDL_DestroyTexture(in_tex);
	if (out_tex)
		SDL_DestroyTexture(out_tex);
	memset(pass, 0, sizeof pass);
	npasses = nluts = 0;
	fbo = 0;
	in_tex = out_tex = NULL;
	in_w = in_h = out_w = out_h = still_w = still_h = 0;
}

/* Whether this window's renderer is one the passes can run in */
int shader_possible(void)
{
	SDL_RendererInfo info;

	return rg_ren && !SDL_GetRendererInfo(rg_ren, &info) && (!strcmp(info.name, "opengl") || !strcmp(info.name, "opengles2"));
}

/* Loads a preset, its passes compiled and its textures read. Returns 0, or -1 when it can't be
   run here, with why on stderr. */
int shader_open(const char *preset)
{
	char dir[1024], file[1100], *text, *line, *next;
	const char *v, *ver;
	saved_t s;
	int i, k, ok = 1;

	shader_close();
	if (!shader_possible() || SDL_RenderFlush(rg_ren))
		return -1;
#define F(ret, name, args) if (!(*(void **)&gl.name = SDL_GL_GetProcAddress("gl" #name))) { fprintf(stderr, "piarcboot: the graphics driver has no gl%s\n", #name); return -1; }
	GLFUNCS
#undef F
	ver = (const char *)gl.GetString(GL_VERSION);
	es = ver && !strncmp(ver, "OpenGL ES", 9) ? atoi(ver + 10) > 2 ? atoi(ver + 10) : 2 : 0;        /* 0, or OpenGL ES's version */
	ver = (const char *)gl.GetString(GL_SHADING_LANGUAGE_VERSION);
	glsl = ver ? (int)(atof(ver) * 100 + 0.5) : 0;

	if (!(text = (char *)file_read(preset, NULL)))
		return -1;
	nkeys = nparams = 0;
	for (line = text; line && *line; line = next)
	{
		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		preset_line(line);
	}
	free(text);
	snprintf(dir, sizeof dir, "%s", preset);
	dir[base_name(dir) - dir] = 0;
	npasses = (v = opt("shaders", 0)) ? atoi(v) : 0;
	if (npasses < 1 || npasses > MAX_PASSES)
	{
		npasses = 0;
		return -1;
	}

	save_state(&s);
	gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);       /* the renderer leaves its last picture's row length */
	/* The preset's textures, as RGBA. Ones with mipmaps have them made. */
	if ((v = opt("textures", 0)))
	{
		char ids[512], *id, *rest;

		snprintf(ids, sizeof ids, "%s", v);
		for (id = strtok_r(ids, ";", &rest); id && nluts < MAX_LUTS; id = strtok_r(NULL, ";", &rest))
		{
			const char *f = opt(id, 0), *wrap;
			char key[96];
			uint8_t *rgba;
			int w, h, mip;

			snprintf(file, sizeof file, "%s%s", dir, f ? f : "");
			if (!f || !(rgba = png_read(file, &w, &h)))
			{
				fprintf(stderr, "piarcboot: %s can't be read\n", file);
				ok = 0;
				break;
			}
			gl.GenTextures(1, &luts[nluts]);
			gl.BindTexture(GL_TEXTURE_2D, luts[nluts]);
			gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
			free(rgba);
			snprintf(key, sizeof key, "%s_mipmap", id);
			if ((mip = opt_true(key, 0)))
				gl.GenerateMipmap(GL_TEXTURE_2D);
			snprintf(key, sizeof key, "%s_wrap_mode", id);
			wrap = opt(key, 0);
			snprintf(key, sizeof key, "%s_linear", id);
			set_sampling(luts[nluts], opt_true(key, 0), wrap && strstr(wrap, "repeat"), mip);
			snprintf(lut_id[nluts++], sizeof *lut_id, "%s", id);
		}
	}

	for (i = 0; i < npasses; i++)           /* every pass's settings, before any is compiled with them */
		if ((v = opt("shader%d", i)))
		{
			snprintf(file, sizeof file, "%s%s", dir, v);
			if ((text = (char *)file_read(file, NULL)))
				read_params(text);
			free(text);
		}
	for (i = 0; ok && i < npasses; i++)
	{
		pass_t *p = &pass[i];
		const char *wrap;
		char name[32];
		int a;

		if (!(v = opt("shader%d", i)))
		{
			ok = 0;
			break;
		}
		snprintf(file, sizeof file, "%s%s", dir, v);
		if (!(p->prog = program(file)))
		{
			ok = 0;
			break;
		}
		/* a scale for both ways, or one for each; with none, the size of what is before it, and the
		   last pass the size it is shown */
		for (a = 0; a < 2; a++)
		{
			p->type[a] = scale_type(opt(a ? "scale_type_y%d" : "scale_type_x%d", i));
			if (p->type[a] < 0)
				p->type[a] = scale_type(opt("scale_type%d", i));
			if (p->type[a] < 0)
				p->type[a] = i == npasses - 1 ? BY_VIEWPORT : BY_SOURCE;
			v = opt(a ? "scale_y%d" : "scale_x%d", i);
			if (!v)
				v = opt("scale%d", i);
			p->scale[a] = v ? (float)atof(v) : 1.0f;
		}
		p->linear = opt_true("filter_linear%d", i);
		if (!(wrap = opt("wrap_mode%d", i)))
			wrap = opt("texture_wrap_mode%d", i);
		p->repeat = wrap && strstr(wrap, "repeat");
		p->srgb = opt_true("srgb_framebuffer%d", i);
		snprintf(p->alias, sizeof p->alias, "%s", (v = opt("alias%d", i)) ? v : "");

		p->mvp = gl.GetUniformLocation(p->prog, "MVPMatrix");
		p->frame_count = gl.GetUniformLocation(p->prog, "FrameCount");
		p->frame_dir = gl.GetUniformLocation(p->prog, "FrameDirection");
		p->out_size = gl.GetUniformLocation(p->prog, "OutputSize");
		add_bind(p, "", i - 1);                /* Texture first, on unit 0 */
		add_bind(p, "Orig", ORIG);
		snprintf(name, sizeof name, "PassPrev%d", i + 1);
		add_bind(p, name, ORIG);
		for (k = 0; k < i; k++)
		{
			snprintf(name, sizeof name, "Pass%d", k + 1);
			add_bind(p, name, k);
			snprintf(name, sizeof name, "PassPrev%d", i - k);
			add_bind(p, name, k);
			if (pass[k].alias[0])
				add_bind(p, pass[k].alias, k);
		}
		for (k = 0; k < nluts && p->nbinds < MAX_BINDS; k++)
			if ((p->bind[p->nbinds].tex = gl.GetUniformLocation(p->prog, lut_id[k])) >= 0)
			{
				p->bind[p->nbinds].tex_size = p->bind[p->nbinds].in_size = -1;
				p->bind[p->nbinds++].from = LUT + k;
			}
		gl.UseProgram(p->prog);
		for (k = 0; k < p->nbinds; k++)
			gl.Uniform1i(p->bind[k].tex, k);
		/* A pass that reads no texture the picture has gone into, nor the frame's number, draws
		   the same every frame, so it is drawn once for a size. CRT-Royale has 3: the beam's
		   table and the 2 that resize the phosphor mask. */
		p->still = p->frame_count < 0 && p->frame_dir < 0;
		for (k = 0; k < p->nbinds; k++)
			if (p->bind[k].tex >= 0 && p->bind[k].from < LUT && (p->bind[k].from == ORIG || !pass[p->bind[k].from].still))
				p->still = 0;
	}
	for (i = 0; ok && i < npasses; i++)     /* every pass has the settings any of them names */
	{
		gl.UseProgram(pass[i].prog);
		for (k = 0; k < nparams; k++)
			gl.Uniform1f(gl.GetUniformLocation(pass[i].prog, param_name[k]), param_val[k]);
	}
	if (ok)
		gl.GenFramebuffers(1, &fbo);
	restore_state(&s, 1);
	if (!ok)
	{
		shader_close();
		return -1;
	}
	return 0;
}

/* ---- Running ---- */

static int sized(int type, float scale, int before, int view)
{
	int n = (int)((type == BY_SOURCE ? before * scale : type == BY_VIEWPORT ? view * scale : scale) + 0.5f);

	return n < 1 ? 1 : n;
}

/* The GL texture the renderer keeps one of its textures in */
static GLuint gl_name(SDL_Texture *t)
{
	GLint id = 0;

	if (!SDL_GL_BindTexture(t, NULL, NULL))
	{
		gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &id);
		SDL_GL_UnbindTexture(t);
	}
	return (GLuint)id;
}

/* An SDL target texture of that size, made again when it isn't, and its GL texture */
static GLuint target(SDL_Texture **t, int *tw, int *th, int w, int h, int linear)
{
	if (!*t || *tw != w || *th != h)
	{
		if (*t)
			SDL_DestroyTexture(*t);
		*t = SDL_CreateTexture(rg_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, w, h);
		*tw = w;
		*th = h;
		if (*t)
			SDL_SetTextureScaleMode(*t, linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
	}
	return *t ? gl_name(*t) : 0;
}

/* Runs the passes on the part src of the picture pic, the last drawing w by h. Where the
   picture's place in the window, dst, is that size, the last pass draws straight into the
   window there and *drawn is 1. Otherwise returns the texture with its drawing, for the
   renderer to draw where the picture goes, or NULL. */
SDL_Texture *shader_run(SDL_Texture *pic, const SDL_Rect *src, int w, int h, const SDL_Rect *dst, int *drawn)
{
	static const GLfloat quad[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	static const GLfloat mvp[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1 };
	static const GLfloat mvp_window[16] = { 2, 0, 0, 0, 0, -2, 0, 0, 0, 0, -1, 0, -1, 1, 0, 1 };   /* the window's top is the top */
	SDL_Texture *was = SDL_GetRenderTarget(rg_ren);
	GLuint in, out = 0, tex;
	saved_t s;
	int i, k, units = 1, bw, bh, ow, oh;
	Uint32 format;
	int tw, th, straight;
	int direct = !was && dst->w == w && dst->h == h && !SDL_GetRendererOutputSize(rg_ren, &ow, &oh);

	*drawn = 0;
	if (!npasses)
		return NULL;
	/* The OpenGL ES renderer holds every texture with red and blue changed over, and puts them
	   right as it draws to the window. The passes are to see the picture's real colours, as
	   they do in OpenGL, or the phosphor mask's red and blue stripes land on the wrong ones.
	   OpenGL ES 3 can change a texture's red and blue over as it is read, which is done to the
	   picture while the passes read it and, for good, to the textures made here. OpenGL ES 2
	   can't: there the passes work on the colours changed over, through textures made here.

	   The passes read the renderer's own texture of the picture when all of it is shown and
	   it is an RGB888 one. Any other is copied first, as is a part of one. */
	if (es == 2)
		direct = 0;
	straight = !SDL_QueryTexture(pic, &format, NULL, &tw, &th) && format == SDL_PIXELFORMAT_RGB888 && !src->x && !src->y && src->w == tw && src->h == th && es != 2;
	if (straight)
		in = gl_name(pic);
	else
	{
		if (!target(&in_tex, &in_w, &in_h, src->w, src->h, 0) || SDL_SetRenderTarget(rg_ren, in_tex))
			return NULL;
		SDL_SetTextureScaleMode(pic, SDL_ScaleModeNearest);
		SDL_RenderCopy(rg_ren, pic, src, NULL);
		SDL_SetRenderTarget(rg_ren, was);
		in = target(&in_tex, &in_w, &in_h, src->w, src->h, 0);
	}
	if (!direct)
		out = target(&out_tex, &out_w, &out_h, w, h, 1);
	if (!in || (!direct && !out) || SDL_RenderFlush(rg_ren))
		return NULL;

	save_state(&s);
	set_sampling(in, pass[0].linear, pass[0].repeat, 0);
	if (es >= 3)
	{
		swizzle(GL_BLUE, GL_RED, GL_ONE);
		if (out)                /* this one the renderer reads, to draw what the passes drew */
		{
			gl.BindTexture(GL_TEXTURE_2D, out);
			swizzle(GL_BLUE, GL_RED, GL_ALPHA);
		}
	}
	gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
	gl.BindBuffer(GL_ARRAY_BUFFER, 0);
	for (i = 0; i < MAX_ATTRIBS; i++)
		gl.DisableVertexAttribArray(i);
	gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, quad);
	gl.EnableVertexAttribArray(0);
	gl.EnableVertexAttribArray(2);
	bw = src->w;
	bh = src->h;
	for (i = 0; i < npasses; i++)
	{
		pass_t *p = &pass[i];
		int last = i == npasses - 1;
		int pw = last ? w : sized(p->type[0], p->scale[0], bw, w), ph = last ? h : sized(p->type[1], p->scale[1], bh, h);

		bw = pw;
		bh = ph;
		p->resized = p->w != pw || p->h != ph;
		p->fresh = !p->still || last || !p->tex || p->resized;
		for (k = 0; k < p->nbinds; k++)         /* drawn again when what it reads was, or a size it is told has changed */
		{
			const bind_t *b = &p->bind[k];

			if (b->from == ORIG ? src->w != still_w || src->h != still_h : b->from < LUT && (pass[b->from].resized || (b->tex >= 0 && pass[b->from].fresh)))
				p->fresh = 1;
		}
		if (!p->fresh)
			continue;
		if (!last && (!p->tex || p->w != pw || p->h != ph))     /* its texture, sampled as the pass after it reads it */
		{
			if (!p->tex)
				gl.GenTextures(1, &p->tex);
			gl.BindTexture(GL_TEXTURE_2D, p->tex);
			gl.TexImage2D(GL_TEXTURE_2D, 0, p->srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			set_sampling(p->tex, pass[i + 1].linear, pass[i + 1].repeat, 0);
		}
		p->w = pw;
		p->h = ph;
		if (!es)                /* OpenGL ES always writes an sRGB texture as sRGB */
			(p->srgb && !last ? gl.Enable : gl.Disable)(GL_FRAMEBUFFER_SRGB);
		if (last && direct)
		{
			gl.BindFramebuffer(GL_FRAMEBUFFER, s.fbo);
			gl.Viewport(dst->x, oh - dst->y - h, w, h);
		}
		else
		{
			gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, last ? out : p->tex, 0);
			gl.Viewport(0, 0, pw, ph);
			gl.Clear(GL_COLOR_BUFFER_BIT);         /* all of it is drawn, so what it held needn't be fetched first */
		}
		gl.UseProgram(p->prog);
		gl.UniformMatrix4fv(p->mvp, 1, GL_FALSE, last && direct ? mvp_window : mvp);
		gl.Uniform1i(p->frame_count, frame);
		gl.Uniform1i(p->frame_dir, 1);
		gl.Uniform2f(p->out_size, (GLfloat)pw, (GLfloat)ph);
		for (k = 0; k < p->nbinds; k++)
		{
			const bind_t *b = &p->bind[k];
			int tw = b->from == ORIG ? src->w : b->from < LUT ? pass[b->from].w : 0;
			int th = b->from == ORIG ? src->h : b->from < LUT ? pass[b->from].h : 0;

			tex = b->from == ORIG ? in : b->from >= LUT ? luts[b->from - LUT] : pass[b->from].tex;
			gl.ActiveTexture(GL_TEXTURE0 + k);
			gl.BindTexture(GL_TEXTURE_2D, tex);
			gl.Uniform2f(b->tex_size, (GLfloat)tw, (GLfloat)th);
			gl.Uniform2f(b->in_size, (GLfloat)tw, (GLfloat)th);
		}
		units = p->nbinds > units ? p->nbinds : units;
		gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	if (straight)           /* the picture's texture as the renderer had it */
	{
		gl.ActiveTexture(GL_TEXTURE0);
		set_sampling(in, 0, 0, 0);
		if (es)
			swizzle(GL_RED, GL_BLUE, GL_ALPHA);
	}
	restore_state(&s, units);
	if (straight)
		SDL_SetTextureScaleMode(pic, SDL_ScaleModeNearest);
	frame++;
	still_w = src->w;
	still_h = src->h;
	*drawn = direct;
	return direct ? NULL : out_tex;
}
