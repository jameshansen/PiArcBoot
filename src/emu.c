/* PiArcBoot: the emulated machine. It is ArcEm's (in arcem/), set up as an A3000 with a 25 MHz
   ARM3 and 4MB, or as an A5000 with its IOEB and 82C711 for the games whose discs are 1.6MB, and
   this is the layer between it and the rest of PiArcBoot: power on, a centisecond at a time, the
   script, the keyboard and mouse, the sound, the CMOS RAM and clock.

   ArcEm counts a cycle for each instruction fetch, each word or byte read or written and each
   internal cycle, and the machine here runs 25,000,000 of them to the second of machine time, a
   cycle for each tick of a 25 MHz ARM3 running from its cache. The rate is fixed, so the IOC's
   timers, the video and the sound follow the cycle count alone and a run is the same on every
   host; the game window paces the steps to real time itself. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL.h>
#include <zlib.h>
#include "../arcem/armdefs.h"
#include "../arcem/eventq.h"
#include "../arcem/arch/ArcemConfig.h"
#include "../arcem/arch/armarc.h"
#include "../arcem/arch/fastmap.h"
#include "../arcem/arch/archio.h"
#include "../arcem/arch/keyboard.h"
#include "../arcem/arch/i2c.h"
#include "../arcem/arch/sound.h"
#include "../arcem/arch/hdc63463.h"
#include "../arcem/arch/filecalls.h"
#include "../arcem/arch/ControlPane.h"
#include "../arcem/arch/dbugsys.h"
#include "piarcboot.h"
#include "core.h"
#include "../arcem/snap.h"
#include "disc.h"

void ARMul_Emulate26(ARMul_State *state);       /* armemu.c */

#define RATE 48000                      /* the sound's rate, in frames a second */
#define QUEUE_MAX (RATE * 4 / 5)        /* 200 ms of 16 bit stereo, in bytes */
/* With the CMOS RAM games start from, RISC OS moves the pointer 3 OS units for each count from
   the mouse. The mouse's movement is kept in half OS units, as many as the pixels of the picture
   it crossed come to in the screen mode (machine_os_per_pixel), so that the pointer goes as far
   across the picture as the mouse did. */
#define MOUSE_STEP 3

int emu_mute;
uint8_t emu_host_keys[512];
int machine_fixed_time;

static ArcemConfig config;
static uint8_t cmos[256];
static int started, tracing;
static uint64_t clock_base;
static CycleCount clock_seen;
static time_t host_time0;
static SDL_AudioDeviceID audio;
static FILE *wav;
static uint32_t wav_frames;

/* ---- Time ---- */

uint64_t machine_clock(void)
{
	ARMul_State *state = &statestr;

	return clock_base + (CycleCount)(ARMul_Time - clock_seen);
}

static void clock_sync(void)
{
	ARMul_State *state = &statestr;

	clock_base += (CycleCount)(ARMul_Time - clock_seen);
	clock_seen = ARMul_Time;
}

/* The real time clock: from 12:00:00 on 1 January 1992 for tests, so that runs repeat, and from
   the host's clock otherwise, running with machine time either way */
static time_t rtc(void)
{
	return (machine_fixed_time ? (time_t)694267200 : host_time0) + (time_t)(machine_clock() / MACHINE_HZ);
}

/* The end of a centisecond: ARMul_Emulate26 returns at the next instruction */
static void step_end(ARMul_State *state, CycleCount now)
{
	UNUSED_VAR(now);
	state->KillEmulator = true;
	EventQ_RescheduleHead(state, state->EventQ[0].Time + MACHINE_CS, step_end);
}

/* ---- The keyboard ---- */

typedef struct
{
	int sc;
	arch_key_id kid;
} sc_key_t;

static const sc_key_t sc_keys[] = {
	{ SDL_SCANCODE_A, ARCH_KEY_a }, { SDL_SCANCODE_B, ARCH_KEY_b }, { SDL_SCANCODE_C, ARCH_KEY_c },
	{ SDL_SCANCODE_D, ARCH_KEY_d }, { SDL_SCANCODE_E, ARCH_KEY_e }, { SDL_SCANCODE_F, ARCH_KEY_f },
	{ SDL_SCANCODE_G, ARCH_KEY_g }, { SDL_SCANCODE_H, ARCH_KEY_h }, { SDL_SCANCODE_I, ARCH_KEY_i },
	{ SDL_SCANCODE_J, ARCH_KEY_j }, { SDL_SCANCODE_K, ARCH_KEY_k }, { SDL_SCANCODE_L, ARCH_KEY_l },
	{ SDL_SCANCODE_M, ARCH_KEY_m }, { SDL_SCANCODE_N, ARCH_KEY_n }, { SDL_SCANCODE_O, ARCH_KEY_o },
	{ SDL_SCANCODE_P, ARCH_KEY_p }, { SDL_SCANCODE_Q, ARCH_KEY_q }, { SDL_SCANCODE_R, ARCH_KEY_r },
	{ SDL_SCANCODE_S, ARCH_KEY_s }, { SDL_SCANCODE_T, ARCH_KEY_t }, { SDL_SCANCODE_U, ARCH_KEY_u },
	{ SDL_SCANCODE_V, ARCH_KEY_v }, { SDL_SCANCODE_W, ARCH_KEY_w }, { SDL_SCANCODE_X, ARCH_KEY_x },
	{ SDL_SCANCODE_Y, ARCH_KEY_y }, { SDL_SCANCODE_Z, ARCH_KEY_z },
	{ SDL_SCANCODE_1, ARCH_KEY_1 }, { SDL_SCANCODE_2, ARCH_KEY_2 }, { SDL_SCANCODE_3, ARCH_KEY_3 },
	{ SDL_SCANCODE_4, ARCH_KEY_4 }, { SDL_SCANCODE_5, ARCH_KEY_5 }, { SDL_SCANCODE_6, ARCH_KEY_6 },
	{ SDL_SCANCODE_7, ARCH_KEY_7 }, { SDL_SCANCODE_8, ARCH_KEY_8 }, { SDL_SCANCODE_9, ARCH_KEY_9 },
	{ SDL_SCANCODE_0, ARCH_KEY_0 },
	{ SDL_SCANCODE_RETURN, ARCH_KEY_return }, { SDL_SCANCODE_ESCAPE, ARCH_KEY_escape },
	{ SDL_SCANCODE_BACKSPACE, ARCH_KEY_backspace }, { SDL_SCANCODE_TAB, ARCH_KEY_tab },
	{ SDL_SCANCODE_SPACE, ARCH_KEY_space }, { SDL_SCANCODE_MINUS, ARCH_KEY_minus },
	{ SDL_SCANCODE_EQUALS, ARCH_KEY_equal }, { SDL_SCANCODE_LEFTBRACKET, ARCH_KEY_bracket_l },
	{ SDL_SCANCODE_RIGHTBRACKET, ARCH_KEY_bracket_r }, { SDL_SCANCODE_BACKSLASH, ARCH_KEY_backslash },
	{ SDL_SCANCODE_NONUSHASH, ARCH_KEY_backslash }, { SDL_SCANCODE_SEMICOLON, ARCH_KEY_semicolon },
	{ SDL_SCANCODE_APOSTROPHE, ARCH_KEY_apostrophe }, { SDL_SCANCODE_GRAVE, ARCH_KEY_grave },
	{ SDL_SCANCODE_COMMA, ARCH_KEY_comma }, { SDL_SCANCODE_PERIOD, ARCH_KEY_period },
	{ SDL_SCANCODE_SLASH, ARCH_KEY_slash }, { SDL_SCANCODE_CAPSLOCK, ARCH_KEY_caps_lock },
	{ SDL_SCANCODE_F1, ARCH_KEY_f1 }, { SDL_SCANCODE_F2, ARCH_KEY_f2 }, { SDL_SCANCODE_F3, ARCH_KEY_f3 },
	{ SDL_SCANCODE_F4, ARCH_KEY_f4 }, { SDL_SCANCODE_F5, ARCH_KEY_f5 }, { SDL_SCANCODE_F6, ARCH_KEY_f6 },
	{ SDL_SCANCODE_F7, ARCH_KEY_f7 }, { SDL_SCANCODE_F8, ARCH_KEY_f8 }, { SDL_SCANCODE_F9, ARCH_KEY_f9 },
	{ SDL_SCANCODE_F10, ARCH_KEY_f10 }, { SDL_SCANCODE_F11, ARCH_KEY_f11 }, { SDL_SCANCODE_F12, ARCH_KEY_f12 },
	{ SDL_SCANCODE_PRINTSCREEN, ARCH_KEY_print }, { SDL_SCANCODE_SCROLLLOCK, ARCH_KEY_scroll_lock },
	{ SDL_SCANCODE_PAUSE, ARCH_KEY_break }, { SDL_SCANCODE_INSERT, ARCH_KEY_insert },
	{ SDL_SCANCODE_HOME, ARCH_KEY_home }, { SDL_SCANCODE_PAGEUP, ARCH_KEY_page_up },
	{ SDL_SCANCODE_DELETE, ARCH_KEY_delete }, { SDL_SCANCODE_END, ARCH_KEY_copy },
	{ SDL_SCANCODE_PAGEDOWN, ARCH_KEY_page_down }, { SDL_SCANCODE_RIGHT, ARCH_KEY_right },
	{ SDL_SCANCODE_LEFT, ARCH_KEY_left }, { SDL_SCANCODE_DOWN, ARCH_KEY_down }, { SDL_SCANCODE_UP, ARCH_KEY_up },
	{ SDL_SCANCODE_NUMLOCKCLEAR, ARCH_KEY_num_lock }, { SDL_SCANCODE_KP_DIVIDE, ARCH_KEY_kp_slash },
	{ SDL_SCANCODE_KP_MULTIPLY, ARCH_KEY_kp_star }, { SDL_SCANCODE_KP_MINUS, ARCH_KEY_kp_minus },
	{ SDL_SCANCODE_KP_PLUS, ARCH_KEY_kp_plus }, { SDL_SCANCODE_KP_ENTER, ARCH_KEY_kp_enter },
	{ SDL_SCANCODE_KP_1, ARCH_KEY_kp_1 }, { SDL_SCANCODE_KP_2, ARCH_KEY_kp_2 }, { SDL_SCANCODE_KP_3, ARCH_KEY_kp_3 },
	{ SDL_SCANCODE_KP_4, ARCH_KEY_kp_4 }, { SDL_SCANCODE_KP_5, ARCH_KEY_kp_5 }, { SDL_SCANCODE_KP_6, ARCH_KEY_kp_6 },
	{ SDL_SCANCODE_KP_7, ARCH_KEY_kp_7 }, { SDL_SCANCODE_KP_8, ARCH_KEY_kp_8 }, { SDL_SCANCODE_KP_9, ARCH_KEY_kp_9 },
	{ SDL_SCANCODE_KP_0, ARCH_KEY_kp_0 }, { SDL_SCANCODE_KP_PERIOD, ARCH_KEY_kp_decimal },
	{ SDL_SCANCODE_NONUSBACKSLASH, ARCH_KEY_backslash },
	{ SDL_SCANCODE_LCTRL, ARCH_KEY_control_l }, { SDL_SCANCODE_LSHIFT, ARCH_KEY_shift_l },
	{ SDL_SCANCODE_LALT, ARCH_KEY_alt_l }, { SDL_SCANCODE_RCTRL, ARCH_KEY_control_r },
	{ SDL_SCANCODE_RSHIFT, ARCH_KEY_shift_r }, { SDL_SCANCODE_RALT, ARCH_KEY_alt_r },
};

/* The keys a character is typed with: the Archimedes keyboard has the US arrangement of shifted symbols */
static int char_key(int ch, arch_key_id *kid, int *shift)
{
	static const char plain[] = "`1234567890-=[]\\;',./", shifted[] = "~!@#$%^&*()_+{}|:\"<>?";
	static const arch_key_id keys[] = {
		ARCH_KEY_grave, ARCH_KEY_1, ARCH_KEY_2, ARCH_KEY_3, ARCH_KEY_4, ARCH_KEY_5, ARCH_KEY_6, ARCH_KEY_7,
		ARCH_KEY_8, ARCH_KEY_9, ARCH_KEY_0, ARCH_KEY_minus, ARCH_KEY_equal, ARCH_KEY_bracket_l, ARCH_KEY_bracket_r,
		ARCH_KEY_backslash, ARCH_KEY_semicolon, ARCH_KEY_apostrophe, ARCH_KEY_comma, ARCH_KEY_period, ARCH_KEY_slash
	};
	const char *p;

	*shift = 0;
	if (ch >= 'a' && ch <= 'z')
		*kid = sc_keys[ch - 'a'].kid;  /* the table starts with the letters, in order */
	else if (ch >= 'A' && ch <= 'Z')
	{
		*kid = sc_keys[ch - 'A'].kid;
		*shift = 1;
	}
	else if (ch == ' ')
		*kid = ARCH_KEY_space;
	else if (ch == '\n')
		*kid = ARCH_KEY_return;
	else if (ch == 27)
		*kid = ARCH_KEY_escape;
	else if (ch == '\t')
		*kid = ARCH_KEY_tab;
	else if (ch && (p = strchr(plain, ch)))
		*kid = keys[p - plain];
	else if (ch && (p = strchr(shifted, ch)))
	{
		*kid = keys[p - shifted];
		*shift = 1;
	}
	else
		return -1;
	return 0;
}

static const struct
{
	const char *name;
	arch_key_id kid;
} key_names[] = {
	{ "enter", ARCH_KEY_return }, { "return", ARCH_KEY_return }, { "esc", ARCH_KEY_escape },
	{ "space", ARCH_KEY_space }, { "shift", ARCH_KEY_shift_l }, { "ctrl", ARCH_KEY_control_l },
	{ "alt", ARCH_KEY_alt_l }, { "delete", ARCH_KEY_delete }, { "break", ARCH_KEY_break },
	{ "tab", ARCH_KEY_tab }, { "backspace", ARCH_KEY_backspace }, { "up", ARCH_KEY_up },
	{ "down", ARCH_KEY_down }, { "left", ARCH_KEY_left }, { "right", ARCH_KEY_right },
	{ "f1", ARCH_KEY_f1 }, { "f2", ARCH_KEY_f2 }, { "f3", ARCH_KEY_f3 }, { "f4", ARCH_KEY_f4 },
	{ "f5", ARCH_KEY_f5 }, { "f6", ARCH_KEY_f6 }, { "f7", ARCH_KEY_f7 }, { "f8", ARCH_KEY_f8 },
	{ "f9", ARCH_KEY_f9 }, { "f10", ARCH_KEY_f10 }, { "f11", ARCH_KEY_f11 }, { "f12", ARCH_KEY_f12 },
	{ "copy", ARCH_KEY_copy }, { "home", ARCH_KEY_home }, { "insert", ARCH_KEY_insert },
	{ "pageup", ARCH_KEY_page_up }, { "pagedown", ARCH_KEY_page_down }, { "capslock", ARCH_KEY_caps_lock },
};

static uint8_t script_keys[ARCH_KEY_count], sent_keys[ARCH_KEY_count];
static int host_buttons, script_buttons;
static int mouse_dx, mouse_dy;          /* movement still to go, in half OS units */

/* Tells the machine's keyboard about every key that has gone down or up since the last time,
   from the real keyboard, the mouse buttons and the script together */
static void keys_update(void)
{
	ARMul_State *state = &statestr;
	uint8_t held[ARCH_KEY_count];
	int buttons = host_buttons | script_buttons, k;
	size_t i;

	memcpy(held, script_keys, sizeof held);
	for (i = 0; i < sizeof sc_keys / sizeof *sc_keys; i++)
		if (emu_host_keys[sc_keys[i].sc])
			held[sc_keys[i].kid] = 1;
	held[ARCH_KEY_button_1] |= (buttons & 1) != 0;      /* Select */
	held[ARCH_KEY_button_3] |= (buttons & 2) != 0;      /* Adjust */
	held[ARCH_KEY_button_2] |= (buttons & 4) != 0;      /* Menu */
	for (k = 0; k < ARCH_KEY_count; k++)
		if (held[k] != sent_keys[k])
		{
			if ((KBD.BuffWritePos + 1) % KBDBUFFLEN == KBD.BuffReadPos)
				break;                  /* the keyboard's queue is full: the rest wait */
			keyboard_key_changed(&KBD, (arch_key_id)k, !held[k]);
			sent_keys[k] = held[k];
		}
}

/* Mouse movement goes to the keyboard 63 counts at most at a time, each way */
static void mouse_update(void)
{
	ARMul_State *state = &statestr;
	int x = (int8_t)(KBD.MouseXCount << 1) >> 1, y = (int8_t)(KBD.MouseYCount << 1) >> 1, take;

	take = mouse_dx / (2 * MOUSE_STEP);
	take = take > 63 - x ? 63 - x : take < -63 - x ? -63 - x : take;
	x += take;
	mouse_dx -= take * 2 * MOUSE_STEP;
	take = -mouse_dy / (2 * MOUSE_STEP);            /* up is positive to the machine */
	take = take > 63 - y ? 63 - y : take < -63 - y ? -63 - y : take;
	y += take;
	mouse_dy += take * 2 * MOUSE_STEP;
	KBD.MouseXCount = (uint8_t)(x & 127);
	KBD.MouseYCount = (uint8_t)(y & 127);
}

int emu_host_xeig = -1, emu_host_yeig = -1;

/* How many half OS units a pixel of the picture is across and down: from the screen mode's eigen
   factors, which ROO311's HostLink gives with each change of mode, and the picture's doubling of a
   narrow mode's pixels and a short one's lines. 4 each way, the usual modes', until it has. */
void machine_os_per_pixel(int *x2, int *y2)
{
	*x2 = emu_host_xeig >= 0 ? (2 << emu_host_xeig) / screen_xscale() : 4;
	*y2 = emu_host_yeig >= 0 ? (2 << emu_host_yeig) / (emu_dbl ? 2 : 1) : 4;
	*x2 = *x2 > 0 ? *x2 : 1;
	*y2 = *y2 > 0 ? *y2 : 1;
}

void emu_mouse(int dx, int dy, int buttons)
{
	int x2, y2;

	machine_os_per_pixel(&x2, &y2);
	mouse_dx += dx * x2;
	mouse_dy += dy * y2;
	host_buttons = buttons;
}

/* ---- The script ---- */

enum { S_WAIT, S_UNTILREAD, S_UNTILERROR, S_UNTILIDLE, S_KEY, S_MOUSE, S_BUTTON, S_DISC, S_SHOT, S_CMOS, S_RUN, S_CLICK, S_POINTER,
	S_SAVESTATE, S_LOADSTATE };

typedef struct
{
	int op, a, b;
	char *s;
} step_t;

static step_t *steps;
static int n_steps, pc, wait_left, until_active, until_left, until_base, *until_count, idle_need, idle_run;

static void add(int op, int a, int b, const char *s)
{
	steps = realloc(steps, (n_steps + 1) * sizeof *steps);
	steps[n_steps].op = op;
	steps[n_steps].a = a;
	steps[n_steps].b = b;
	steps[n_steps].s = s ? strdup(s) : NULL;
	n_steps++;
}

static int find_key(const char *name, arch_key_id *kid)
{
	size_t i;
	int shift;

	for (i = 0; i < sizeof key_names / sizeof *key_names; i++)
		if (!strcmp(name, key_names[i].name))
		{
			*kid = key_names[i].kid;
			return 0;
		}
	if (name[0] && !name[1])
		return char_key(tolower((unsigned char)name[0]), kid, &shift);
	return -1;
}

/* Each character: Shift down and 3 cs if it needs Shift, the key down for 5 cs and up for 3,
   then Shift up and 3 cs */
static void type_text(const char *t)
{
	for (; *t; t++)
	{
		int ch = (unsigned char)*t, shift;
		arch_key_id kid;

		if (ch == '\\' && t[1])
		{
			t++;
			ch = *t == 'n' ? '\n' : *t == 'e' ? 27 : *t == 't' ? '\t' : (unsigned char)*t;
		}
		if (char_key(ch, &kid, &shift))
		{
			fprintf(stderr, "piarcboot: no key types '%c'\n", ch);
			continue;
		}
		if (shift)
		{
			add(S_KEY, ARCH_KEY_shift_l, 1, NULL);
			add(S_WAIT, 3, 0, NULL);
		}
		add(S_KEY, kid, 1, NULL);
		add(S_WAIT, 5, 0, NULL);
		add(S_KEY, kid, 0, NULL);
		add(S_WAIT, 3, 0, NULL);
		if (shift)
		{
			add(S_KEY, ARCH_KEY_shift_l, 0, NULL);
			add(S_WAIT, 3, 0, NULL);
		}
	}
}

void emu_script_line(const char *line)
{
	char cmd[32] = "", a[1024] = "", b[1024] = "";
	const char *rest;
	arch_key_id kid;
	int n;

	while (*line == ' ' || *line == '\t')
		line++;
	if (!*line || *line == '#' || *line == '\r' || *line == '\n')
		return;
	sscanf(line, "%31s", cmd);
	rest = line + strlen(cmd);
	if (*rest == ' ')
		rest++;
	n = sscanf(rest, "%1023s %1023[^\r\n]", a, b);
	if (!strcmp(cmd, "wait") && n >= 1)
		add(S_WAIT, atoi(a), 0, NULL);
	else if (!strcmp(cmd, "untilread") && n >= 1)
		add(S_UNTILREAD, atoi(a), 0, NULL);
	else if (!strcmp(cmd, "untilerror") && n >= 1)  /* HostLink: until an error box opens, as for a disc to put in */
		add(S_UNTILERROR, atoi(a), 0, NULL);
	else if (!strcmp(cmd, "untilidle") && n == 2)    /* until the floppy drive has been still for a cs, or b cs have gone */
		add(S_UNTILIDLE, atoi(a), atoi(b), NULL);
	else if (!strcmp(cmd, "key") && n == 2 && !find_key(a, &kid))
		add(S_KEY, kid, !strcmp(b, "down"), NULL);
	else if (!strcmp(cmd, "type"))
	{
		char text[1024];

		snprintf(text, sizeof text, "%s", rest);
		text[strcspn(text, "\r\n")] = 0;
		type_text(text);
	}
	else if (!strcmp(cmd, "mouse") && n == 2)
		add(S_MOUSE, atoi(a), atoi(b), NULL);
	else if (!strcmp(cmd, "pointer") && n == 2)      /* HostLink: the pointer to there, in OS units */
		add(S_POINTER, atoi(a), atoi(b), NULL);
	else if (!strcmp(cmd, "button") && n == 2 && strchr("lmr", a[0]))
		add(S_BUTTON, a[0] == 'l' ? 1 : a[0] == 'r' ? 2 : 4, !strcmp(b, "down"), NULL);
	else if ((!strcmp(cmd, "disc") || !strcmp(cmd, "discp")) && n == 2)
		add(S_DISC, atoi(a), cmd[4] == 'p', b);
	else if (!strcmp(cmd, "shot") && n >= 1)
		add(S_SHOT, 0, 0, rest);
	else if (!strcmp(cmd, "cmos") && n >= 1)
		add(S_CMOS, 0, 0, rest);
	else if (!strcmp(cmd, "run") && n >= 1)         /* HostLink: start a *command as a task */
		add(S_RUN, 0, 0, rest);
	else if (!strcmp(cmd, "click") && n >= 1)       /* HostLink: click the icon bar icon with that sprite */
		add(S_CLICK, 0, 0, rest);
	else if (!strcmp(cmd, "savestate") && n >= 1)   /* the machine to a file, once no floppy command is going */
		add(S_SAVESTATE, 0, 0, rest);
	else if (!strcmp(cmd, "loadstate") && n >= 1)   /* and back from one */
		add(S_LOADSTATE, 0, 0, rest);
	else
		fprintf(stderr, "piarcboot: a script step that makes no sense: %s\n", line);
}

void emu_script(const char *text)
{
	while (text && *text)
	{
		char line[2048];
		size_t len = strcspn(text, "\n");

		snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line ? len : sizeof line - 1), text);
		emu_script_line(line);
		text += len;
		if (*text)
			text++;
	}
}

int emu_script_busy(void)
{
	return pc < n_steps || wait_left > 0 || until_active;
}

/* Carries out the steps due at the start of a centisecond, up to one that waits */
static void script_run(void)
{
	for (;;)
	{
		step_t *s;

		if (wait_left > 0)
		{
			wait_left--;
			return;
		}
		if (until_active)
		{
			idle_run = machine_motor_on() ? 0 : idle_run + 1;
			if ((until_count ? *until_count != until_base : idle_run >= idle_need) || until_left <= 0)
				until_active = 0;
			else
			{
				until_left--;
				return;
			}
		}
		if (pc >= n_steps)
			return;
		s = &steps[pc++];
		switch (s->op)
		{
			case S_WAIT:
			wait_left = s->a;
			break;

			case S_UNTILREAD:
			case S_UNTILERROR:
			until_active = 1;
			until_left = s->a;
			until_count = s->op == S_UNTILREAD ? &machine_disc_reads : &emu_host_errors;
			until_base = *until_count;
			break;

			case S_UNTILIDLE:
			until_active = 1;
			until_left = s->b;
			until_count = NULL;
			idle_need = s->a;
			idle_run = 0;
			break;

			case S_KEY:
			script_keys[s->a] = (uint8_t)s->b;
			break;

			case S_MOUSE:
			emu_mouse(s->a, s->b, host_buttons);
			break;

			case S_BUTTON:
			script_buttons = s->b ? script_buttons | s->a : script_buttons & ~s->a;
			break;

			case S_DISC:
			if (machine_disc(s->a, s->s, s->b))
				fprintf(stderr, "piarcboot: can't read the disc image %s\n", s->s);
			break;

			case S_SHOT:
			emu_save_shot(s->s);
			break;

			case S_RUN:
			emu_host_run(s->s);
			break;

			case S_CLICK:
			emu_host_click(s->s);
			break;

			case S_POINTER:
			emu_host_pointer(s->a, s->b);
			break;

			case S_SAVESTATE:
			case S_LOADSTATE:
			{
				char why[200];

				if (!state_ready())
				{
					pc--;           /* a floppy command is going: the next centisecond */
					return;
				}
				if (s->op == S_SAVESTATE ? state_save(s->s, 1, NULL, 0, 0) : state_load(s->s, why, sizeof why))
					fprintf(stderr, "piarcboot: can't %s the state %s: %s\n", s->op == S_SAVESTATE ? "save" : "restore", s->s,
						s->op == S_SAVESTATE ? "it couldn't be written" : why);
				break;
			}

			case S_CMOS:
			{
				uint8_t now[256];

				I2C_GetCMOS(now);
				if (file_write(s->s, now, sizeof now))
					fprintf(stderr, "piarcboot: can't write %s\n", s->s);
				break;
			}
		}
	}
}

/* ---- Sound ---- */

static SoundFrame sound_buf[4096];

static void wav_header(void)
{
	uint8_t h[44];
	uint32_t bytes = wav_frames * 4, v[11] = { 0 };
	int i;

	memcpy(h, "RIFF", 4);
	memcpy(h + 8, "WAVEfmt ", 8);
	memcpy(h + 36, "data", 4);
	v[1] = 36 + bytes;
	v[4] = 16;
	v[6] = RATE;
	v[7] = RATE * 4;
	v[10] = bytes;
	for (i = 1; i < 11; i++)
		if (i != 2 && i != 3 && i != 9)
		{
			h[i * 4] = (uint8_t)v[i];
			h[i * 4 + 1] = (uint8_t)(v[i] >> 8);
			h[i * 4 + 2] = (uint8_t)(v[i] >> 16);
			h[i * 4 + 3] = (uint8_t)(v[i] >> 24);
		}
	h[20] = 1;                      /* PCM */
	h[21] = 0;
	h[22] = 2;                      /* stereo */
	h[23] = 0;
	h[32] = 4;                      /* bytes a frame */
	h[33] = 0;
	h[34] = 16;                     /* bits a sample */
	h[35] = 0;
	fseek(wav, 0, SEEK_SET);
	fwrite(h, 1, sizeof h, wav);
	fseek(wav, 0, SEEK_END);
}

SoundFrame *Sound_GetHostBuffer(size_t *destavail)
{
	*destavail = sizeof sound_buf / sizeof *sound_buf;
	return sound_buf;
}

/* ArcEm rounds the ratio between the machine's sample rate and 48 kHz, so it makes a little more
   or less than 48,000 frames a second of machine time (Lemmings' 64 us period gives about 48,200),
   and the host's sound clock drifts from the timer the game window keeps time by. Queued as they
   come, the frames would pile up to the limit and be dropped a batch at a time, or run dry. So
   they are resampled on the way to the host, by a ratio that keeps about QUEUE_AIM queued: at most
   a hundredth either way, and steady once the queue has settled. */
#define QUEUE_AIM (RATE * 4 / 10 * 4 / 5)      /* 80 ms, in bytes */
static SoundFrame rs_out[sizeof sound_buf / sizeof *sound_buf * 102 / 100], rs_prev;
static double rs_pos, rs_level = QUEUE_AIM;
static unsigned sound_dropped, sound_dry;
static int rise_at, rise_len;           /* frames into the sound rising from nothing to full, and how many that takes */

/* The sound from here starts silent and rises to full over that many milliseconds, so a game
   doesn't start with a jump in volume */
void emu_audio_fade_in(int ms)
{
	rise_len = RATE / 1000 * ms;
	rise_at = 0;
}

static void queue_sound(const SoundFrame *in, size_t frames)
{
	Uint32 queued = SDL_GetQueuedAudioSize(audio);
	double step;
	size_t n = 0;

	if (!queued)
	{
		static const SoundFrame quiet[RATE / 25];      /* 40 ms of silence to start from, or after running dry */

		sound_dry++;
		SDL_QueueAudio(audio, quiet, sizeof quiet);
		queued = sizeof quiet;
	}
	/* The queue goes down in steps of the host's whole buffers, so it is smoothed over about a third
	   of a second, and the ratio moves a fortieth as far as the queue is from its aim */
	rs_level += (queued - rs_level) / 64;
	step = 1 + (rs_level - QUEUE_AIM) / QUEUE_AIM / 40;   /* frames in for each frame out */
	step = step < 0.99 ? 0.99 : step > 1.01 ? 1.01 : step;
	/* rs_pos counts from the batch's first frame, with the last one of the batch before at -1 */
	for (; rs_pos < (double)frames - 1 && n < sizeof rs_out / sizeof *rs_out; rs_pos += step)
	{
		int i = (int)(rs_pos + 1) - 1, c;
		double f = rs_pos - i;
		const SoundFrame *a = i < 0 ? &rs_prev : &in[i], *b = &in[i + 1];

		double gain = rise_at < rise_len ? (double)rise_at++ / rise_len : 1;

		for (c = 0; c < 2; c++)
			rs_out[n].channel[c] = (SoundData)((a->channel[c] + (b->channel[c] - a->channel[c]) * f) * gain);
		n++;
	}
	rs_pos = rs_pos - frames < -1 ? -1 : rs_pos - frames;
	rs_prev = in[frames - 1];
	if (queued + n * 4 <= QUEUE_MAX)
		SDL_QueueAudio(audio, rs_out, (Uint32)(n * 4));
	else
		sound_dropped += (unsigned)n;
}

void Sound_HostBuffered(SoundFrame *buffer, size_t frames)
{
	if (wav)
	{
		fwrite(buffer, sizeof *buffer, frames, wav);
		wav_frames += (uint32_t)frames;
	}
	if (audio && !emu_mute && frames)
		queue_sound(buffer, frames);
}

bool Sound_InitHost(ARMul_State *state)
{
	const char *path = getenv("PIARCBOOT_WAV");

	UNUSED_VAR(state);
	eSound_StereoSense = Stereo_LeftRight;
	Sound_BatchSize = 16;
	Sound_HostRate = RATE << 10;
	if (path && (wav = fopen(path, "wb")))
		wav_header();
	return true;
}

void Sound_ShutdownHost(ARMul_State *state)
{
	UNUSED_VAR(state);
}

void emu_audio_open(void)
{
	SDL_AudioSpec want, have;

	if (audio)
		return;
	memset(&want, 0, sizeof want);
	want.freq = RATE;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (audio)
		SDL_PauseAudioDevice(audio, 0);
}

/* ---- What ArcEm asks of a front end ---- */

bool ControlPane_Init(ARMul_State *state)
{
	UNUSED_VAR(state);
	return true;
}

void ControlPane_Error(bool fatal, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "piarcboot: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	if (fatal)
		exit(EXIT_FAILURE);
}

/* ArcEm's own notes, shown with PIARCBOOT_LOG set */
void log_msgv(int type, const char *format, va_list ap)
{
	static int show = -1;

	UNUSED_VAR(type);
	if (show < 0)
		show = getenv("PIARCBOOT_LOG") != NULL;
	if (show)
		vfprintf(stderr, format, ap);
}

int Kbd_PollHostKbd(ARMul_State *state)
{
	UNUSED_VAR(state);
	return 0;
}

FILE *File_OpenAppData(const char *sName, const char *sMode)
{
	UNUSED_VAR(sName);
	UNUSED_VAR(sMode);
	return NULL;
}

/* The extension ROM's modules come only from the folder emu_extrom_dir names */
Directory *Directory_OpenAppDir(const char *sName)
{
	UNUSED_VAR(sName);
	return NULL;
}

/* An A3000 has no hard disc controller: its addresses read as nothing is there */
void HDC_Init(ARMul_State *state)
{
	UNUSED_VAR(state);
}

void HDC_Regular(ARMul_State *state)
{
	UNUSED_VAR(state);
}

uint_fast16_t HDC_Read(ARMul_State *state, uint_fast16_t offset)
{
	UNUSED_VAR(state);
	UNUSED_VAR(offset);
	return 0xffff;
}

void HDC_Write(ARMul_State *state, uint_fast16_t offset, uint_fast16_t data)
{
	UNUSED_VAR(state);
	UNUSED_VAR(offset);
	UNUSED_VAR(data);
}

/* ---- HostLink ----

   ROO311's HostLink module works the desktop for the game window: it starts an application or
   clicks an icon bar icon when asked, puts the pointer where it is told, and says when the
   screen mode changes. It reaches PiArcBoot through four SWIs in ArcEm's chunk, which ArcEm
   passes to ARMul_HostSWI: Hello (it is there), Poll (anything to do?), Event (what happened)
   and Pointer (where should the pointer be?). */

enum { HL_HELLO = 0x20, HL_POLL, HL_EVENT, HL_POINTER };

int emu_hostlink, emu_host_modes, emu_host_clicked, emu_host_missed, emu_host_errors, emu_host_tasks, emu_host_error_open;
int emu_host_nwins, emu_host_wins[31][4];
int emu_host_modes_at_run = -1, emu_host_errors_at_run = -1, emu_host_tasks_at_run = -1, emu_host_vidc_at_run = -1;
static struct
{
	int code;
	char text[256];
} hl_queue[16];
static int hl_count, hl_px = -1, hl_py = -1, hl_moved;

static void hl_add(int code, const char *text)
{
	if (hl_count == 16)
		return;
	hl_queue[hl_count].code = code;
	snprintf(hl_queue[hl_count].text, sizeof hl_queue[0].text, "%s", text);
	hl_count++;
}

void emu_host_run(const char *command)
{
	hl_add(1, command);
}

void emu_host_click(const char *sprite)
{
	hl_add(2, sprite);
}

void emu_host_forget(void)
{
	hl_count = 0;
}

void emu_host_pointer(int x, int y)
{
	if (x != hl_px || y != hl_py)
		hl_moved = 1;
	hl_px = x;
	hl_py = y;
}

static bool hostlink_swi(ARMul_State *state, unsigned number)
{
	size_t i;

	switch (number)
	{
		case HL_HELLO:
		emu_hostlink = 1;
		state->Reg[0] = 1;
		return true;

		case HL_POLL:   /* the next command, its text written to the module's buffer */
		{
			ARMword buf = state->Reg[0], size = state->Reg[1];

			state->Reg[0] = 0;
			if (hl_count && size > 0)
			{
				const char *t = hl_queue[0].text;

				for (i = 0; i < size - 1 && t[i]; i++)
					ARMul_StoreByte(state, buf + i, (uint8_t)t[i]);
				ARMul_StoreByte(state, buf + i, 0);
				state->Reg[0] = (ARMword)hl_queue[0].code;
				if (hl_queue[0].code == 1)      /* where the module's counts stood as it took up a command to start */
				{
					emu_host_modes_at_run = emu_host_modes;
					emu_host_errors_at_run = emu_host_errors;
					emu_host_tasks_at_run = emu_host_tasks;
					emu_host_vidc_at_run = machine_mode_sets;
				}
				memmove(hl_queue, hl_queue + 1, --hl_count * sizeof *hl_queue);
			}
			return true;
		}

		case HL_EVENT:
		if (state->Reg[0] == 1)         /* a new mode, with its eigen factors */
		{
			emu_host_modes++;
			emu_host_xeig = state->Reg[2] <= 3 ? (int)state->Reg[2] : -1;
			emu_host_yeig = state->Reg[3] <= 3 ? (int)state->Reg[3] : -1;
		}
		else if (state->Reg[0] == 2)
			emu_host_clicked++;
		else if (state->Reg[0] == 3)
			emu_host_missed++;
		else if (state->Reg[0] == 4)    /* an error box opened, or with R1 0 closed */
		{
			emu_host_errors += state->Reg[1] == 1;
			emu_host_error_open = state->Reg[1] == 1;
		}
		else if (state->Reg[0] == 5)
			emu_host_tasks++;
		else if (state->Reg[0] == 6)    /* the windows on the screen: how many, then each one's outline */
		{
			ARMword at = state->Reg[1], n = ARMul_LoadWordS(state, at);

			n = n > 31 ? 0 : n;
			for (i = 0; i < n * 4; i++)
				emu_host_wins[i / 4][i % 4] = (int)ARMul_LoadWordS(state, at + 4 + (ARMword)i * 4);
			emu_host_nwins = (int)n;
			for (i = 0; tracing && i < n; i++)
				fprintf(stderr, "hostlink window %d %d %d %d\n", emu_host_wins[i][0], emu_host_wins[i][1], emu_host_wins[i][2], emu_host_wins[i][3]);
		}
		if (tracing)
			fprintf(stderr, "hostlink event %u %u\n", (unsigned)state->Reg[0], (unsigned)state->Reg[1]);
		return true;

		case HL_POINTER:
		state->Reg[0] = hl_moved && hl_px >= 0;
		state->Reg[1] = (ARMword)hl_px;
		state->Reg[2] = (ARMword)hl_py;
		if (tracing && hl_moved)
			fprintf(stderr, "hostlink pointer %d %d\n", hl_px, hl_py);
		hl_moved = 0;
		return true;
	}
	return false;
}

/* ---- Power on, and a centisecond at a time ---- */

const char *emu_extrom_dir, *emu_hostfs_dir;

/* A3000 speed: each instruction fetch counts this many cycles of the 25 MHz machine, which with
   the rest of an instruction's cycles comes to about 4.7 million instructions a second, an 8 MHz
   ARM2's. Starch's attract mode, timed against a recording of it, runs within 1% over its three
   minutes, the demos within a third of a second. The timers, video and sound keep machine time,
   so only the processor slows. */
#define A3000_FETCH 5
int emu_a3000;

int emu_start(const char *rom, const uint8_t *cmos_in, int fdc)
{
	ARMul_State *state;
	FILE *f = fopen(rom, "rb");

	if (!f)
		return 1;
	fclose(f);
	if (cmos_in)
		memcpy(cmos, cmos_in, sizeof cmos);
	I2C_CMOSInMemory = true;
	I2C_CMOSData = cmos;
	I2C_HostTime = rtc;
	host_time0 = time(NULL);
	tracing = getenv("PIARCBOOT_TRACE") != NULL;

	memset(&config, 0, sizeof config);
	config.eMemSize = MemSize_4M;
	config.eProcessor = Processor_ARM3;
	config.sRomImageName = strdup(rom);
	config.sEXTNDirectory = strdup(emu_extrom_dir ? emu_extrom_dir : "");
	config.sHostFSDirectory = strdup(emu_hostfs_dir ? emu_hostfs_dir : "");	config.bAspectRatioCorrection = true;
	config.bUpscale = true;
	ARMul_FixedEmuRate = ARMul_EmuRate = MACHINE_HZ;
	ARMul_FetchCycles = emu_a3000 ? A3000_FETCH : 1;
	ARMul_HostSWI = hostlink_swi;
	c711_start(fdc == 1);

	state = ARMul_NewState(&config);
	if (!state)
		return 1;
	EventQ_Insert(state, ARMul_Time + MACHINE_CS, step_end);
	clock_seen = ARMul_Time;
	clock_base = 0;
	started = 1;
	return 0;
}

void emu_step(void)
{
	ARMul_State *state = &statestr;
	static int cs, last_modes = -1, last_reads = -1, last_motor = -1;

	if (!started)
		return;
	script_run();
	keys_update();
	mouse_update();
	ARMul_Emulate26(state);
	clock_sync();
	cs++;
	if (tracing && (machine_mode_sets != last_modes || machine_disc_reads != last_reads || fdc_motor() != last_motor))
	{
		last_modes = machine_mode_sets;
		last_reads = machine_disc_reads;
		last_motor = fdc_motor();
		fprintf(stderr, "%d cs: mode sets %d, disc reads %d, motor %s\n", cs, last_modes, last_reads, last_motor ? "on" : "off");
	}
}

/* ---- Save states ---- */

void UpdateTimerRegisters_Event(ARMul_State *state, CycleCount nowtime);       /* arch/archio.c */
void FDCHDC_Poll(ARMul_State *state, CycleCount nowtime);
void Sound_DMAEvent(ARMul_State *state, CycleCount nowtime);                    /* arch/newsound.c */
extern const EventQ_Func screen_events[];                                       /* screen.c */

/* The events the machine can have waiting, which a save state names by number: their place in
   this list, or from 16 the display driver's in the order screen.c gives them */
static EventQ_Func event_func(int32_t n)
{
	const EventQ_Func fixed[] = { step_end, UpdateTimerRegisters_Event, FDCHDC_Poll, Keyboard_Poll, Sound_DMAEvent };
	int i;

	if (n >= 0 && n < (int32_t)(sizeof fixed / sizeof *fixed))
		return fixed[n];
	for (i = 0; n >= 16 && screen_events[i]; i++)
		if (n == 16 + i)
			return screen_events[i];
	return NULL;
}

static int32_t event_number(EventQ_Func f)
{
	int32_t n;

	for (n = 0; n < 32; n++)
		if (event_func(n) && event_func(n) == f)
			return n;
	return -1;
}

/* The processor and the events waiting for it, the clock, and what this layer has told the
   machine: the keys it holds down and what HostLink has said. The script and the host's own keys
   and mouse aren't part of it, and a script carries on from a state read back. */
void emu_snap(snap_t *s)
{
	ARMul_State *state = &statestr;
	int i;

	SNAP(s, state->Reg);
	SNAP(s, state->NumCycles);
	SNAP(s, state->NextInstr);
	SNAP(s, state->Bank);
	SNAP(s, state->OSmode);
	SNAP(s, state->NtransSig);
	SNAP(s, state->abortSig);
	SNAP(s, state->Aborted);
	SNAP(s, state->AbortAddr);
	SNAP(s, state->Exception);
	SNAP(s, state->Base);
	SNAP(s, state->HasSWP);
	SNAP(s, state->HasCP15);
	SNAP(s, state->instr);
	SNAP(s, state->pc);
	SNAP(s, state->loaded);
	SNAP(s, state->decoded);
	SNAP(s, state->RegBank);
	SNAP(s, state->NumEvents);
	if (state->NumEvents > EVENTQ_SIZE)
	{
		s->bad = 1;
		return;
	}
	for (i = 0; i < state->NumEvents; i++)
	{
		int32_t n = s->saving ? event_number(state->EventQ[i].Func) : 0;

		SNAP(s, state->EventQ[i].Time);
		SNAP(s, n);
		if (s->saving ? n < 0 : !(state->EventQ[i].Func = event_func(n)))
			s->bad = 1;
	}
	SNAP(s, clock_base);
	SNAP(s, clock_seen);
	SNAP(s, sent_keys);
	SNAP(s, emu_hostlink);
	SNAP(s, emu_host_modes);
	SNAP(s, emu_host_clicked);
	SNAP(s, emu_host_missed);
	SNAP(s, emu_host_errors);
	SNAP(s, emu_host_tasks);
	SNAP(s, emu_host_nwins);
	SNAP(s, emu_host_wins);
	SNAP(s, emu_host_xeig);
	SNAP(s, emu_host_yeig);
	SNAP(s, hl_px);
	SNAP(s, hl_py);
	if (s->saving)
		return;
	if (emu_host_nwins < 0 || emu_host_nwins > 31)
		emu_host_nwins = 0;
	emu_host_error_open = 0;        /* not kept in a state, so that older states still read */
	mouse_dx = mouse_dy = 0;
	hl_count = 0;
	hl_moved = hl_px >= 0;
}

/* Drops the steps still to come, and lets go of the keys and buttons they hold */
void emu_script_stop(void)
{
	while (n_steps > 0)
		free(steps[--n_steps].s);
	pc = wait_left = until_active = 0;
	memset(script_keys, 0, sizeof script_keys);
	script_buttons = 0;
}

/* ---- The rest of the machine as src/ sees it ---- */

/* A CMOS RAM with a desktop mode and monitor type of its own in place of Auto, and its checksum
   made again: 1 and the rest of its bytes added up, at physical &3F. Logical location n is at
   physical n + &40, and &C0 to &EF at &10 to &3F. */
void machine_cmos_desktop(uint8_t *c, int mode, int monitor)
{
	int i, sum = 1;

	c[0x13] &= ~0x10;                       /* &C3, bit 4: the Wimp mode isn't Auto */
	c[0x14] = (uint8_t)mode;                /* &C4: the Wimp mode */
	c[0xc5] = (uint8_t)(monitor << 2 | (c[0xc5] & 0x02));   /* &85: the monitor type, with separate syncs */
	for (i = 0x10; i < 0x3f; i++)
		sum += c[i];
	for (i = 0x40; i < 0x100; i++)
		sum += c[i];
	c[0x3f] = (uint8_t)sum;
}

int machine_disc(int drive, const char *path, int write_protect)
{
	return fdc_insert(drive, path, write_protect);
}

int machine_motor_on(void)
{
	return fdc_motor();
}

const char *machine_disc_path(int drive)
{
	return drive >= 0 && drive < 4 && floppy[drive].disc ? floppy[drive].disc->path : NULL;
}

void machine_close(void)
{
	fdc_close();
	if (wav)
	{
		wav_header();
		fclose(wav);
		wav = NULL;
	}
	if (audio)
	{
		if (getenv("PIARCBOOT_RUN_FOR"))
			fprintf(stderr, "sound: %u frames dropped, ran dry %u times%c", sound_dropped, sound_dry, 10);
		SDL_CloseAudioDevice(audio);
		audio = 0;
	}
}

/* What the machine is now, to tell the end of two runs apart: its memory, its registers and its clock */
void emu_state(char *buf, size_t size)
{
	ARMul_State *state = &statestr;
	uLong regs = crc32(0, (const Bytef *)state->Reg, sizeof state->Reg);

	regs = crc32(regs, (const Bytef *)state->RegBank, sizeof state->RegBank);
	snprintf(buf, size, "state ram=%08lx regs=%08lx clock=%016llx", (unsigned long)crc32(0, (const Bytef *)MEMC.PhysRam, MEMC.RAMSize),
		 (unsigned long)regs, (unsigned long long)machine_clock());
}
