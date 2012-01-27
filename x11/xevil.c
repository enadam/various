/*
 * xevil.c -- show what is happening to X windows in real time
 *
 * This program is like xev, but
 *   -- xevil can decode known XClientMessageEvent:s,
 *   -- on PropertyNewValue events it prints the new property value,
 *   -- its output consist of more condensed yet more readable one-line
 *      summaries of the events,
 *   -- you can follow multiple windows simultanously.
 *
 * On the other hand, xev knows more events, while xevil only deals with
 * ClientMessage:s, Create/Destroy:s, Map/Unmap/ConfigureNotify:es,
 * Expose/Visility:es, KeyPress/Release:s, damages and window shape changes.
 *
 * xevil depends on xlib and glib.  Compile it with:
 * cc -Wall `pkg-config --cflags --libs x11 glib-2.0` -lXdamage -lXext xevil.c
 *
 * Synopsis:
 *   xevil [-t] [[+|-]{<event>}]... [<window>]...
 *
 * Possible <event>s are: create, map, config, shape, prop, ipc, visibility,
 * expose, damage, ptr, kbd.  If no events are specified all of them are
 * tracked except for exposures, damages and pointer crossing events.
 * With -t the output is prefixed with timestamps.
 *
 * <window>s are the XID:s of the windows you would like to track.
 * If omitted the default is the root window.
 */

/* Include files */
#include <stdlib.h>
#include <assert.h>

#include <string.h>
#include <stdio.h>
#include <getopt.h>
#include <sys/time.h>

#include <X11/Xmd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xdamage.h>

#include <glib.h>

/* Private variables */
static int Error;
static Atom XA_utf8_string, XA_wm_state;
static int Opt_timestamp;

/* Program code */
/* Postprocess/print $str. */
static void output(GString *str, Bool synthetic)
{
	if (Opt_timestamp)
	{
		struct timeval tv;

		gettimeofday(&tv, NULL);
		printf(synthetic
		       ? "%lu.%.6lu %s (synthetic)\n" : "%lu.%.6lu %s\n",
		       tv.tv_sec, tv.tv_usec, str->str);
	} else if (synthetic)
		printf("%s (synthetic)\n", str->str);
	else
		puts(str->str);
} /* output */

/* Print X errors but don't die. */
static int xerror_handler(Display *dpy, XErrorEvent *ev)
{
	fprintf(stderr, "XError %u, request: %u/%u, resource: 0x%lx\n",
		ev->error_code, ev->request_code, ev->minor_code,
		ev->resourceid);
	Error = ev->error_code;
	return 0;
} /* xerror_handler */

/* Get the name of $atom and cache it. */
static char *get_atom(Display *dpy, Atom atom)
{
	static GHashTable *cache;
	char *name;

	if (!atom)
		return "None";
	if (!cache)
	{	/* Make sure we can store Atom:s in $cache. */
		typedef int ass[-!(sizeof(gint) == sizeof(atom))];
		cache = g_hash_table_new(g_int_hash, g_int_equal);
	} else if ((name = g_hash_table_lookup(cache, &atom)) != NULL)
		/* $cache hit */
		return name;

	Error = 0;
	name = XGetAtomName(dpy, atom);
	if (Error)
	{
		name = g_strdup_printf("%lx", atom);
	} else if (name[0] == '\0')
	{
		XFree(name);
		name = "\"\"";
	} else if (strspn(name, " ,"))
	{
		char *tmp;
		name = g_strdup_printf("\"%s\"", tmp = name);
		XFree(tmp);
	}

	g_hash_table_insert(cache, &atom, name);
	return name;
} /* get_atom */

/* Translate @state and add to @line. */
static void fmtwmstate(Display *dpy, long state, GString *line)
{
	static char const *states[]
		= { "Withdrawn", "Normal", "2", "Iconic" };

	if (0 <= state && state < G_N_ELEMENTS(states))
		g_string_append_printf(line, "%sState",
			states[state]);
	else
		g_string_append_printf(line, "%ld", state);
} /* fmtwmstate */

/* Print at the end of $line $n property $val:ues of type $t. */
static void fmtprop(Display *dpy, Atom t, unsigned n, void const *val,
	GString *line)
{
	unsigned i;

	if (t == XA_STRING || t == XA_utf8_string)
	{
		char const *str;

		g_string_append_c(line, '"');
		for (str = (char *)val; n > 0; str++, n--)
		{
			if (*str == '\0')
				g_string_append(line, "\",\"");
			else if (*str == '"')
				g_string_append(line, "\\\"");
			else	// This is not UTF-safe.
				g_string_append_c(line, *str);
		}
		g_string_append_c(line, '"');
		return;
	}

	if (n == 0)
	{
		g_string_append(line, "<empty>");
		return;
	} else if (t == XA_wm_state)
	{
		fmtwmstate(dpy, *(CARD32 *)val, line);
		return;
	}

	if (n > 1)
		g_string_append_c(line, '[');
	for (i = 0; ;)
	{
		if (t == XA_ATOM)
			g_string_append(line, get_atom(dpy,
				((Atom *)val)[i]));
		else if (t == XA_WINDOW)
			g_string_append_printf(line,
				"0x%lx", ((Window *)val)[i]);
		else if (t == XA_INTEGER)
			g_string_append_printf(line,
				"%d", ((int *)val)[i]);
		else if (t == XA_CARDINAL)
			g_string_append_printf(line,
				"%u", ((unsigned *)val)[i]);

		if (++i >= n)
			break;
		g_string_append(line, ", ");
	}
	if (n > 1)
		g_string_append_c(line, ']');
} /* fmtprop */

/* Pretty print $win into $str, right-padding it so that the next field
 * can be aligned. */
static void fmtxid(GString *str, Window win)
{
	unsigned indent, i;

	/* How many spaces to pad with? */
	for (indent = 1, i = 0x10000000; win < i; indent++, i >>= 4)
		;
	g_string_printf(str, "0x%lx:%*s", win, indent, "");
} /* fmtxid */

/* Print an XPropertyEvent into $line.  If it's changed try to print
 * the new value as well. */
static void property_event(Display *dpy, XPropertyEvent const *ev,
	GString *line)
{
	char *name;
	Atom t;
	int fmt;
	unsigned long n, b;
	unsigned char *val;

	fmtxid(line, ev->window);
	name = get_atom(dpy, ev->atom);
	if (ev->state == PropertyDelete)
	{
		g_string_append_printf(line, "%s deleted", name);
		goto out0;
	} else if (XGetWindowProperty(dpy, ev->window,
		ev->atom, 0, ~0, False, AnyPropertyType,
		&t, &fmt, &n, &b, &val) != Success)
	{	/* Couldn't get the new value, it could be already deleted. */
		g_string_append_printf(line, "%s changed", name);
		goto out0;
	} else if ((t != XA_ATOM && t != XA_WINDOW
		&& t != XA_INTEGER && t != XA_CARDINAL
		&& t != XA_STRING && t != XA_utf8_string
		&& t != XA_wm_state))
	{	/* We don't know this type. */
		g_string_append_printf(line, "%s changed", name);
		goto out1;
	}

	/* Print the new value. */
	g_string_append(line, name);
	g_string_append_c(line, '=');
	fmtprop(dpy, t, n, val, line);

out1:
	XFree(val);
out0:
	output(line, ev->send_event);
} /* property_event */

/* Print an XClientMessageEvent.  Decode the parameters if we know the
 * message type. */
static void client_message(Display *dpy, XClientMessageEvent const *msg,
	GString *line)
{
	char *msgtype;
	unsigned n, i;

	fmtxid(line, msg->window);
	msgtype = get_atom(dpy, msg->message_type);
	g_string_append(line, msgtype);

	/* Decode the parameters if we know the message type. */
	i = 0;
	g_string_append_c(line, '(');
	if (!strcmp(msgtype, "_NET_WM_STATE") && msg->format == 32)
	{
		static char const *actions[] = { "Remove", "Add", "Toggle" };
		long const *args = msg->data.l;
		long action;

		action = args[i++];
		if (0 <= action && action < G_N_ELEMENTS(actions))
			g_string_append_printf(line, "%s, ", actions[action]);
		else
			g_string_append_printf(line, "%ld, ", action);

		g_string_append_printf(line, "%s, ",
			get_atom(dpy, args[i++]));
		g_string_append_printf(line, "%s, ",
			get_atom(dpy, args[i++]));
	} else if (!strcmp(msgtype, "WM_PROTOCOLS") && msg->format == 32)
	{
		char const *action;
		long const *args = msg->data.l;

		action = get_atom(dpy, args[i++]);
		g_string_append_printf(line, "%s, %ld, ", action, args[i++]);
		if (!strcmp(action, "_NET_WM_PING"))
			g_string_append(line, "win=");
	} else if (!strcmp(msgtype, "WM_CHANGE_STATE") && msg->format == 32)
	{
		long const *args = msg->data.l;
		fmtwmstate(dpy, args[i++], line);
		g_string_append(line, ", ");
	} else if (!strcmp(msgtype, "_NET_ACTIVE_WINDOW") && msg->format == 32)
	{
		static char const *sources[]
			= { "OldClient", "App", "Pager" };
		long const *args = msg->data.l;
		long source;

		source = args[i++];
		if (0 <= source && source < G_N_ELEMENTS(sources))
			g_string_append_printf(line, "ReqBy%s, ",
				sources[source]);
		else
			g_string_append_printf(line, "%ld, ", source);
		g_string_append_printf(line, "%ld, requestor=", args[i++]);
	} /* if */

	/* Just dump the rest of the parameters. */
	switch (msg->format)
	{
	case 8:
		n = G_N_ELEMENTS(msg->data.b);
		for (; i < n; i++)
			g_string_append_printf(line,
				i+1 < n ? "0x%x, " : "0x%x",
				msg->data.b[i]);
		break;
	case 16:
		n = G_N_ELEMENTS(msg->data.s);
		for (; i < n; i++)
			g_string_append_printf(line,
				i+1 < n ? "0x%x, " : "0x%x",
				msg->data.s[i]);
		break;
	case 32:
		n = G_N_ELEMENTS(msg->data.l);
		for (; i < n; i++)
			g_string_append_printf(line,
				i+1 < n ? "0x%lx, " : "0x%lx",
				msg->data.l[i]);
		break;
	} /* switch */

	g_string_append_c(line, ')');
	output(line, False);
} /* client_message */

/* The main function */
int main(int argc, char const *argv[])
{
	Display *dpy;
	GString *line;
	unsigned xeventmask;
	int error_base, shape_event, damage_event;
	struct
	{
		Bool children, creation, mapping, configure, shape;
		Bool properties, clientmsg;
		Bool visibility, exposure, damages;
		Bool pointer, keyboard;
	} track;

	dpy = XOpenDisplay(NULL);
	XSetErrorHandler(xerror_handler);
	XA_utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
	XA_wm_state    = XInternAtom(dpy, "WM_STATE", False);

	if (argv[1] && !strcmp(argv[1], "-t"))
	{
		Opt_timestamp = 1;
		optind++;
	}

	/* Choose which events we're interested in. */
	memset(&track, 0, sizeof(track));
	track.children		= True;
	track.creation		= True;
	track.mapping		= True;
	track.configure		= True;
	track.shape		= True;
	track.properties	= True;
	track.clientmsg		= True;
	track.visibility	= True;
	track.keyboard		= True;
	for (; argv[optind]; optind++)
	{
		char const *opt;
		Bool add, del, *which;

		opt = argv[optind];
		add = opt[0] == '+';
		del = opt[0] == '-';
		if (add || del)
			opt++;

		if (!strcmp(opt, "children"))
			which = &track.children;
		else if (!strcmp(opt, "create"))
			which = &track.creation;
		else if (!strcmp(opt, "map"))
			which = &track.mapping;
		else if (!strcmp(opt, "config"))
			which = &track.configure;
		else if (!strcmp(opt, "shape"))
			which = &track.shape;
		else if (!strcmp(opt, "prop"))
			which = &track.properties;
		else if (!strcmp(opt, "ipc"))
			which = &track.clientmsg;
		else if (!strcmp(opt, "visibility"))
			which = &track.visibility;
		else if (!strcmp(opt, "expose"))
			which = &track.exposure;
		else if (!strcmp(opt, "damage"))
			which = &track.damages;
		else if (!strcmp(opt, "ptr"))
			which = &track.pointer;
		else if (!strcmp(opt, "kbd"))
			which = &track.keyboard;
		else
			break;

		if (!add && !del)
			memset(&track, 0, sizeof(track));
		*which = !del;
	} /* for */

	xeventmask = 0;
	if (track.creation || track.mapping || track.configure
			|| track.clientmsg)
		xeventmask |= track.children
			? SubstructureNotifyMask
			: StructureNotifyMask;
	if (track.shape)
		XShapeQueryExtension(dpy, &shape_event, &error_base);
	if (track.properties)
		xeventmask |= PropertyChangeMask;
	if (track.visibility)
		xeventmask |= VisibilityChangeMask;
	if (track.exposure)
		xeventmask |= ExposureMask;
	if (track.damages)
		XDamageQueryExtension(dpy, &damage_event, &error_base);
	if (track.pointer);
		xeventmask |= EnterWindowMask|LeaveWindowMask;
	if (track.keyboard)
		xeventmask |= KeyPressMask|KeyReleaseMask;

	/* XSelectInput() the windows we're interested in
	 * or the root window. */
	if (argv[optind])
		do
		{
			Window win;
			char const *errp;

			win = strtoul(argv[optind], (char **)&errp, 0);
			if (errp == argv[optind] || *errp)
			{
				fprintf(stderr, "%s: what is `%s'?\n",
					argv[0], argv[optind]);
				exit(1);
			}

			XSelectInput(dpy, win, xeventmask);
			if (track.shape)
				XShapeSelectInput(dpy, win, ShapeNotifyMask);
			if (track.damages)
				XDamageCreate(dpy, win,
					XDamageReportRawRectangles);
		} while (argv[++optind]);
	else
		XSelectInput(dpy, DefaultRootWindow(dpy), xeventmask);

	/* The main loop */
	line = g_string_new("");
	for (;;)
	{
		XEvent ev;

		/* Wait for, get and process the next event. */
		XNextEvent(dpy, &ev);
		if (ev.type == CreateNotify)
		{
			XCreateWindowEvent const *create = &ev.xcreatewindow;

			if (!track.creation)
				continue;
			fmtxid(line, create->parent);
			g_string_append_printf(line,
				"Create(0x%lx)", create->window);
			output(line, ev.xany.send_event);
		} else if (ev.type == DestroyNotify)
		{
			XDestroyWindowEvent const *destroy = &ev.xdestroywindow;

			if (!track.creation)
				continue;
			fmtxid(line, destroy->event);
			g_string_append_printf(line,
				"Destroy(0x%lx)", destroy->window);
			output(line, ev.xany.send_event);
		} else if (ev.type == MapNotify)
		{
			XMapEvent const *map = &ev.xmap;

			if (!track.mapping)
				continue;
			fmtxid(line, map->event);
			g_string_append_printf(line, "Map(0x%lx%s)",
				map->window, map->override_redirect
					? ", override_redirected" : "");
			output(line, ev.xany.send_event);
		} else if (ev.type == UnmapNotify)
		{
			XUnmapEvent const *unmap = &ev.xunmap;

			if (!track.mapping)
				continue;
			fmtxid(line, unmap->event);
			g_string_append_printf(line, "Unmap(0x%lx%s)",
				unmap->window, unmap->from_configure
					? ", from_configure" : "");
			output(line, ev.xany.send_event);
		} else if (ev.type == ReparentNotify)
		{
			XReparentEvent const *reparent  = &ev.xreparent;

			if (!track.configure)
				continue;
			fmtxid(line, reparent->event);
			g_string_append_printf(line,
				"Reparent(0x%lx => 0x%lx)",
				reparent->window, reparent->parent);
			output(line, ev.xany.send_event);
		} else if (ev.type == ConfigureNotify)
		{
			XConfigureEvent const *cfg = &ev.xconfigure;

			if (!track.configure)
				continue;
			fmtxid(line, cfg->event);
			g_string_append_printf(line,
				"Configure(0x%lx => %dx%d%+d%+d, "
				"above=0x%lx%s)", cfg->window,
				cfg->width, cfg->height, cfg->x, cfg->y,
				cfg->above, cfg->override_redirect
					? ", override_redirected" : "");
			output(line, ev.xany.send_event);
		} else if (track.shape && ev.type == shape_event + ShapeNotify)
		{
			static char const *shapes[] =
				{ "Bounding", "Clip", "Input" };
			XShapeEvent sev;

			memcpy(&sev, &ev, sizeof(sev));
			fmtxid(line, sev.window);
			g_string_append_printf(line,
				"Shape(%s => %dx%d%+d%+d)",
				shapes[sev.kind],
				sev.width, sev.height, sev.x, sev.y);
			output(line, ev.xany.send_event);
		} else if (ev.type == PropertyNotify)
		{
			assert(track.properties);
			property_event(dpy, &ev.xproperty, line);
		} else if (ev.type == ClientMessage)
		{
			if (!track.clientmsg)
				continue;
			client_message(dpy, &ev.xclient, line);
		} else if (ev.type == VisibilityNotify)
		{
			static char const *visibilities[] =
			{
				"unobscured",
				"partially obscured",
				"fully obscured",
			};
			XVisibilityEvent const *vis = &ev.xvisibility;

			assert(track.visibility);
			fmtxid(line, vis->window);
			g_string_append_printf(line, "Visibility=%s",
				visibilities[vis->state]);
			output(line, ev.xany.send_event);
		} else if (ev.type == Expose)
		{
			XExposeEvent const *ex = &ev.xexpose;

			assert(track.exposure);
			fmtxid(line, ex->window);
			g_string_append_printf(line,
				"Expose(%dx%d%+d%+d)",
				ex->width, ex->height,
				ex->x, ex->y);
			output(line, ev.xany.send_event);
		} else if (track.damages && ev.type == damage_event)
		{
			XDamageNotifyEvent dev;

			memcpy(&dev, &ev, sizeof(dev));
			fmtxid(line, dev.drawable);
			g_string_append_printf(line,
				"Damage(%dx%d%+d%+d)",
				dev.area.width, dev.area.height,
				dev.area.x, dev.area.y);
			output(line, ev.xany.send_event);
			XDamageSubtract(dpy, dev.damage, None, None);
		} else if (ev.type == EnterNotify || ev.type == LeaveNotify)
		{
			XCrossingEvent const *cross = &ev.xcrossing;

			if (!track.pointer)
				continue;
			fmtxid(line, cross->window);
			g_string_append_printf(line,
				"%s(%dx%d",
				cross->type == EnterNotify
					? "Enter" : "Leave",
				cross->x, cross->y);
			if (cross->mode == NotifyGrab)
				g_string_append(line, ", grab");
			else if (cross->mode == NotifyUngrab)
				g_string_append(line, ", ungrab");
			g_string_append_c(line, ')');
			output(line, ev.xany.send_event);
		} else if (ev.type == KeyPress || ev.type == KeyRelease)
		{
			static struct { int mask; char const *name; } states[] =
			{
				{ ShiftMask,	"Shift"	},
				{ LockMask,	"Lock"	},
				{ ControlMask,	"Ctrl"	},
				{ Mod1Mask,	"Mod1"	},
				{ Mod2Mask,	"Mod2"	},
				{ Mod3Mask,	"Mod3"	},
				{ Mod4Mask,	"Mod4"	},
				{ Mod5Mask,	"Mod5"	},
			};
			unsigned i;
			int has_modifiers;
			XKeyEvent const *key;

			assert(track.keyboard);
			key = &ev.xkey;
			fmtxid(line, key->window);

			/* Prepend with the list of modifiers. */
			has_modifiers = 0;
			for (i = 0; i < G_N_ELEMENTS(states); i++)
				if (key->state & states[i].mask)
				{
					if (!has_modifiers)
					{
						g_string_append_c(line, ' ');
						has_modifiers = 1;
					} else
						g_string_append_c(line, '-');
					g_string_append(line, states[i].name);
				}
                        if (has_modifiers)
				g_string_append_c(line, '-');
			g_string_append_printf(line, "%s %s",
				XKeysymToString(XKeycodeToKeysym(dpy,
						      key->keycode, 0)),
				ev.type == KeyPress
					? "pressed" : "released");
			output(line, ev.xany.send_event);
		} /* if */
	} /* for ever */

	return 0;
} /* main */

/* vim: set noet ts=8 sts=8 sw=8: */
/* End of xevil.c */
