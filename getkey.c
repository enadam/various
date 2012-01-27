/*
 * getkey.c -- lists or tests which evdev keys are being pressed
 *
 * Finds out which keys being pressed and either prints or tests them,
 * depending on the command line.  The list of keys is retrieved from
 * the evdev driver, which assumes a linux kernel and that driver being
 * present.  Nevertheless, it should run correctly on any architecture.
 *
 * Usage
 * -----
 *
 * getkey [-d <fname>]
 *	Lists which keys are being pressed in decimal, hexa,
 *	and symbolic forms.
 *	<fname> is the path to the keyboard evdev device node,
 *	which defaults to /dev/input/event0 (event2 for __ARMEL__).
 * getkey [-d <fname>] {-o|<sym>...}...
 *	Examines the list of keys being pressed and exits with
 *	an appripriate code.  The command line is divided into
 *	blocks, separated by "-o" arguments.  If all keys in a
 *	block are being pressed the program exits with the
 *	corresponding code.  The exit code starts from zero,
 *	and is incremented by each block.  The case of <sym>s 
 *	is ignored, and they don't have to include the "KEY_"
 *	and "BTN_" prefix.  The list of recognized symbols can
 *	be found in <linux/input.h>.
 *
 *	Examples:
 *
 *	``getkey leftshift rightshift'' exits with 0 if both key
 *	are being pressed, and returns 1 otherwise.
 *
 *	``getkey leftshift -o rightshift'' exits with 0 if the left
 *	shift is pressed, with 1 if the right shift is pressed, and
 *	with 2 otherwise.
 *
 *	``getkey -o leftshift rightshift -o escape'' exits with 1
 *	if both the left and right shift keys are pressed, with 2
 *	if the escape key is pressed, and with 3 otherwise.
 *
 * Errors are indicated by error code 255.
 */
/* Include files */
#include <stdlib.h>
#include <values.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>

#include <sys/ioctl.h>
#include <linux/input.h>

/* Macros */
/*
 * -- WIDTH_OF(var):	storage size of `var' in bits
 * -- MEMBS_OF(ary):	# of members in array `ary'
 * -- BITMAP(nbits):	returns the minimal # of bytes 
 *			that can store `nbits' bits
 * -- BIT_OF(ary, i):	returns whether the `i'th bit
 *			of `ary' is set
 */
#define WIDTH_OF(var)	(sizeof(var) * CHAR_BIT)
#define MEMBS_OF(ary)	(sizeof(ary) / sizeof(ary[0]))
#define BITMAP(nbits)	(((nbits) % CHAR_BIT)			\
				? (nbits) / CHAR_BIT + 1	\
				: (nbits) / CHAR_BIT)
#define BIT_OF(ary, i)	(((ary[(i) / WIDTH_OF(ary[0])])		\
				>> ((i) % WIDTH_OF(ary[0]))) & 1)

/* Private constants */
/*
 * Translation table between KEY_* and BTN_* constants defined
 * in <linux/input.h> and their symbolic names.  Some constants
 * have multiple names; for them we omitted all but the first.
 *
 * Symbols[<bit position in the mask>] = <symbolic name>
 * The kernel docs say the constants are architecture independent.
 *
 * This type of designated initialization is a gcc extension.
 */
static char const *const Symbols[] =
{	/* {{{ */
	[KEY_RESERVED]		= "KEY_RESERVED",
	[KEY_ESC]		= "KEY_ESC",
	[KEY_1]			= "KEY_1",
	[KEY_2]			= "KEY_2",
	[KEY_3]			= "KEY_3",
	[KEY_4]			= "KEY_4",
	[KEY_5]			= "KEY_5",
	[KEY_6]			= "KEY_6",
	[KEY_7]			= "KEY_7",
	[KEY_8]			= "KEY_8",
	[KEY_9]			= "KEY_9",
	[KEY_0]			= "KEY_0",
	[KEY_MINUS]		= "KEY_MINUS",
	[KEY_EQUAL]		= "KEY_EQUAL",
	[KEY_BACKSPACE]		= "KEY_BACKSPACE",
	[KEY_TAB]		= "KEY_TAB",
	[KEY_Q]			= "KEY_Q",
	[KEY_W]			= "KEY_W",
	[KEY_E]			= "KEY_E",
	[KEY_R]			= "KEY_R",
	[KEY_T]			= "KEY_T",
	[KEY_Y]			= "KEY_Y",
	[KEY_U]			= "KEY_U",
	[KEY_I]			= "KEY_I",
	[KEY_O]			= "KEY_O",
	[KEY_P]			= "KEY_P",
	[KEY_LEFTBRACE]		= "KEY_LEFTBRACE",
	[KEY_RIGHTBRACE]	= "KEY_RIGHTBRACE",
	[KEY_ENTER]		= "KEY_ENTER",
	[KEY_LEFTCTRL]		= "KEY_LEFTCTRL",
	[KEY_A]			= "KEY_A",
	[KEY_S]			= "KEY_S",
	[KEY_D]			= "KEY_D",
	[KEY_F]			= "KEY_F",
	[KEY_G]			= "KEY_G",
	[KEY_H]			= "KEY_H",
	[KEY_J]			= "KEY_J",
	[KEY_K]			= "KEY_K",
	[KEY_L]			= "KEY_L",
	[KEY_SEMICOLON]		= "KEY_SEMICOLON",
	[KEY_APOSTROPHE]	= "KEY_APOSTROPHE",
	[KEY_GRAVE]		= "KEY_GRAVE",
	[KEY_LEFTSHIFT]		= "KEY_LEFTSHIFT",
	[KEY_BACKSLASH]		= "KEY_BACKSLASH",
	[KEY_Z]			= "KEY_Z",
	[KEY_X]			= "KEY_X",
	[KEY_C]			= "KEY_C",
	[KEY_V]			= "KEY_V",
	[KEY_B]			= "KEY_B",
	[KEY_N]			= "KEY_N",
	[KEY_M]			= "KEY_M",
	[KEY_COMMA]		= "KEY_COMMA",
	[KEY_DOT]		= "KEY_DOT",
	[KEY_SLASH]		= "KEY_SLASH",
	[KEY_RIGHTSHIFT]	= "KEY_RIGHTSHIFT",
	[KEY_KPASTERISK]	= "KEY_KPASTERISK",
	[KEY_LEFTALT]		= "KEY_LEFTALT",
	[KEY_SPACE]		= "KEY_SPACE",
	[KEY_CAPSLOCK]		= "KEY_CAPSLOCK",
	[KEY_F1]		= "KEY_F1",
	[KEY_F2]		= "KEY_F2",
	[KEY_F3]		= "KEY_F3",
	[KEY_F4]		= "KEY_F4",
	[KEY_F5]		= "KEY_F5",
	[KEY_F6]		= "KEY_F6",
	[KEY_F7]		= "KEY_F7",
	[KEY_F8]		= "KEY_F8",
	[KEY_F9]		= "KEY_F9",
	[KEY_F10]		= "KEY_F10",
	[KEY_NUMLOCK]		= "KEY_NUMLOCK",
	[KEY_SCROLLLOCK]	= "KEY_SCROLLLOCK",
	[KEY_KP7]		= "KEY_KP7",
	[KEY_KP8]		= "KEY_KP8",
	[KEY_KP9]		= "KEY_KP9",
	[KEY_KPMINUS]		= "KEY_KPMINUS",
	[KEY_KP4]		= "KEY_KP4",
	[KEY_KP5]		= "KEY_KP5",
	[KEY_KP6]		= "KEY_KP6",
	[KEY_KPPLUS]		= "KEY_KPPLUS",
	[KEY_KP1]		= "KEY_KP1",
	[KEY_KP2]		= "KEY_KP2",
	[KEY_KP3]		= "KEY_KP3",
	[KEY_KP0]		= "KEY_KP0",
	[KEY_KPDOT]		= "KEY_KPDOT",

	[KEY_ZENKAKUHANKAKU]	= "KEY_ZENKAKUHANKAKU",
	[KEY_102ND]		= "KEY_102ND",
	[KEY_F11]		= "KEY_F11",
	[KEY_F12]		= "KEY_F12",
	[KEY_RO]		= "KEY_RO",
	[KEY_KATAKANA]		= "KEY_KATAKANA",
	[KEY_HIRAGANA]		= "KEY_HIRAGANA",
	[KEY_HENKAN]		= "KEY_HENKAN",
	[KEY_KATAKANAHIRAGANA]	= "KEY_KATAKANAHIRAGANA",
	[KEY_MUHENKAN]		= "KEY_MUHENKAN",
	[KEY_KPJPCOMMA]		= "KEY_KPJPCOMMA",
	[KEY_KPENTER]		= "KEY_KPENTER",
	[KEY_RIGHTCTRL]		= "KEY_RIGHTCTRL",
	[KEY_KPSLASH]		= "KEY_KPSLASH",
	[KEY_SYSRQ]		= "KEY_SYSRQ",
	[KEY_RIGHTALT]		= "KEY_RIGHTALT",
	[KEY_LINEFEED]		= "KEY_LINEFEED",
	[KEY_HOME]		= "KEY_HOME",
	[KEY_UP]		= "KEY_UP",
	[KEY_PAGEUP]		= "KEY_PAGEUP",
	[KEY_LEFT]		= "KEY_LEFT",
	[KEY_RIGHT]		= "KEY_RIGHT",
	[KEY_END]		= "KEY_END",
	[KEY_DOWN]		= "KEY_DOWN",
	[KEY_PAGEDOWN]		= "KEY_PAGEDOWN",
	[KEY_INSERT]		= "KEY_INSERT",
	[KEY_DELETE]		= "KEY_DELETE",
	[KEY_MACRO]		= "KEY_MACRO",
	[KEY_MUTE]		= "KEY_MUTE",
	[KEY_VOLUMEDOWN]	= "KEY_VOLUMEDOWN",
	[KEY_VOLUMEUP]		= "KEY_VOLUMEUP",
	[KEY_POWER]		= "KEY_POWER",
	[KEY_KPEQUAL]		= "KEY_KPEQUAL",
	[KEY_KPPLUSMINUS]	= "KEY_KPPLUSMINUS",
	[KEY_PAUSE]		= "KEY_PAUSE",

	[KEY_KPCOMMA]		= "KEY_KPCOMMA",
#ifdef KEY_HANGEUL
	[KEY_HANGEUL]		= "KEY_HANGEUL",
#endif
	[KEY_HANJA]		= "KEY_HANJA",
	[KEY_YEN]		= "KEY_YEN",
	[KEY_LEFTMETA]		= "KEY_LEFTMETA",
	[KEY_RIGHTMETA]		= "KEY_RIGHTMETA",
	[KEY_COMPOSE]		= "KEY_COMPOSE",

	[KEY_STOP]		= "KEY_STOP",
	[KEY_AGAIN]		= "KEY_AGAIN",
	[KEY_PROPS]		= "KEY_PROPS",
	[KEY_UNDO]		= "KEY_UNDO",
	[KEY_FRONT]		= "KEY_FRONT",
	[KEY_COPY]		= "KEY_COPY",
	[KEY_OPEN]		= "KEY_OPEN",
	[KEY_PASTE]		= "KEY_PASTE",
	[KEY_FIND]		= "KEY_FIND",
	[KEY_CUT]		= "KEY_CUT",
	[KEY_HELP]		= "KEY_HELP",
	[KEY_MENU]		= "KEY_MENU",
	[KEY_CALC]		= "KEY_CALC",
	[KEY_SETUP]		= "KEY_SETUP",
	[KEY_SLEEP]		= "KEY_SLEEP",
	[KEY_WAKEUP]		= "KEY_WAKEUP",
	[KEY_FILE]		= "KEY_FILE",
	[KEY_SENDFILE]		= "KEY_SENDFILE",
	[KEY_DELETEFILE]	= "KEY_DELETEFILE",
	[KEY_XFER]		= "KEY_XFER",
	[KEY_PROG1]		= "KEY_PROG1",
	[KEY_PROG2]		= "KEY_PROG2",
	[KEY_WWW]		= "KEY_WWW",
	[KEY_MSDOS]		= "KEY_MSDOS",
	[KEY_COFFEE]		= "KEY_COFFEE",
	[KEY_DIRECTION]		= "KEY_DIRECTION",
	[KEY_CYCLEWINDOWS]	= "KEY_CYCLEWINDOWS",
	[KEY_MAIL]		= "KEY_MAIL",
	[KEY_BOOKMARKS]		= "KEY_BOOKMARKS",
	[KEY_COMPUTER]		= "KEY_COMPUTER",
	[KEY_BACK]		= "KEY_BACK",
	[KEY_FORWARD]		= "KEY_FORWARD",
	[KEY_CLOSECD]		= "KEY_CLOSECD",
	[KEY_EJECTCD]		= "KEY_EJECTCD",
	[KEY_EJECTCLOSECD]	= "KEY_EJECTCLOSECD",
	[KEY_NEXTSONG]		= "KEY_NEXTSONG",
	[KEY_PLAYPAUSE]		= "KEY_PLAYPAUSE",
	[KEY_PREVIOUSSONG]	= "KEY_PREVIOUSSONG",
	[KEY_STOPCD]		= "KEY_STOPCD",
	[KEY_RECORD]		= "KEY_RECORD",
	[KEY_REWIND]		= "KEY_REWIND",
	[KEY_PHONE]		= "KEY_PHONE",
	[KEY_ISO]		= "KEY_ISO",
	[KEY_CONFIG]		= "KEY_CONFIG",
	[KEY_HOMEPAGE]		= "KEY_HOMEPAGE",
	[KEY_REFRESH]		= "KEY_REFRESH",
	[KEY_EXIT]		= "KEY_EXIT",
	[KEY_MOVE]		= "KEY_MOVE",
	[KEY_EDIT]		= "KEY_EDIT",
	[KEY_SCROLLUP]		= "KEY_SCROLLUP",
	[KEY_SCROLLDOWN]	= "KEY_SCROLLDOWN",
	[KEY_KPLEFTPAREN]	= "KEY_KPLEFTPAREN",
	[KEY_KPRIGHTPAREN]	= "KEY_KPRIGHTPAREN",
#ifdef KEY_NEW
	[KEY_NEW]		= "KEY_NEW",
#endif
#ifdef KEY_REDO
	[KEY_REDO]		= "KEY_REDO",
#endif

	[KEY_F13]		= "KEY_F13",
	[KEY_F14]		= "KEY_F14",
	[KEY_F15]		= "KEY_F15",
	[KEY_F16]		= "KEY_F16",
	[KEY_F17]		= "KEY_F17",
	[KEY_F18]		= "KEY_F18",
	[KEY_F19]		= "KEY_F19",
	[KEY_F20]		= "KEY_F20",
	[KEY_F21]		= "KEY_F21",
	[KEY_F22]		= "KEY_F22",
	[KEY_F23]		= "KEY_F23",
	[KEY_F24]		= "KEY_F24",

	[KEY_PLAYCD]		= "KEY_PLAYCD",
	[KEY_PAUSECD]		= "KEY_PAUSECD",
	[KEY_PROG3]		= "KEY_PROG3",
	[KEY_PROG4]		= "KEY_PROG4",
	[KEY_SUSPEND]		= "KEY_SUSPEND",
	[KEY_CLOSE]		= "KEY_CLOSE",
	[KEY_PLAY]		= "KEY_PLAY",
	[KEY_FASTFORWARD]	= "KEY_FASTFORWARD",
	[KEY_BASSBOOST]		= "KEY_BASSBOOST",
	[KEY_PRINT]		= "KEY_PRINT",
	[KEY_HP]		= "KEY_HP",
	[KEY_CAMERA]		= "KEY_CAMERA",
	[KEY_SOUND]		= "KEY_SOUND",
	[KEY_QUESTION]		= "KEY_QUESTION",
	[KEY_EMAIL]		= "KEY_EMAIL",
	[KEY_CHAT]		= "KEY_CHAT",
	[KEY_SEARCH]		= "KEY_SEARCH",
	[KEY_CONNECT]		= "KEY_CONNECT",
	[KEY_FINANCE]		= "KEY_FINANCE",
	[KEY_SPORT]		= "KEY_SPORT",
	[KEY_SHOP]		= "KEY_SHOP",
	[KEY_ALTERASE]		= "KEY_ALTERASE",
	[KEY_CANCEL]		= "KEY_CANCEL",
	[KEY_BRIGHTNESSDOWN]	= "KEY_BRIGHTNESSDOWN",
	[KEY_BRIGHTNESSUP]	= "KEY_BRIGHTNESSUP",
	[KEY_MEDIA]		= "KEY_MEDIA",

	[KEY_SWITCHVIDEOMODE]	= "KEY_SWITCHVIDEOMODE",
	[KEY_KBDILLUMTOGGLE]	= "KEY_KBDILLUMTOGGLE",
	[KEY_KBDILLUMDOWN]	= "KEY_KBDILLUMDOWN",
	[KEY_KBDILLUMUP]	= "KEY_KBDILLUMUP",

#ifdef KEY_SEND
	[KEY_SEND]		= "KEY_SEND",
#endif
#ifdef KEY_REPLY
	[KEY_REPLY]		= "KEY_REPLY",
#endif
#ifdef KEY_FORWARDMAIL
	[KEY_FORWARDMAIL]	= "KEY_FORWARDMAIL",
#endif
#ifdef KEY_SAVE
	[KEY_SAVE]		= "KEY_SAVE",
#endif
#ifdef KEY_DOCUMENTS
	[KEY_DOCUMENTS]		= "KEY_DOCUMENTS",
#endif

#ifdef KEY_BATTERY
	[KEY_BATTERY]		= "KEY_BATTERY",
#endif

#ifdef KEY_BLUETOOTH
	[KEY_BLUETOOTH]		= "KEY_BLUETOOTH",
#endif
#ifdef KEY_WLAN
	[KEY_WLAN]		= "KEY_WLAN",
#endif

	[KEY_UNKNOWN]		= "KEY_UNKNOWN",

	[BTN_MISC]		= "BTN_MISC",
	[BTN_0]			= "BTN_0",
	[BTN_1]			= "BTN_1",
	[BTN_2]			= "BTN_2",
	[BTN_3]			= "BTN_3",
	[BTN_4]			= "BTN_4",
	[BTN_5]			= "BTN_5",
	[BTN_6]			= "BTN_6",
	[BTN_7]			= "BTN_7",
	[BTN_8]			= "BTN_8",
	[BTN_9]			= "BTN_9",

	[BTN_MOUSE]		= "BTN_MOUSE",
	[BTN_LEFT]		= "BTN_LEFT",
	[BTN_RIGHT]		= "BTN_RIGHT",
	[BTN_MIDDLE]		= "BTN_MIDDLE",
	[BTN_SIDE]		= "BTN_SIDE",
	[BTN_EXTRA]		= "BTN_EXTRA",
	[BTN_FORWARD]		= "BTN_FORWARD",
	[BTN_BACK]		= "BTN_BACK",
	[BTN_TASK]		= "BTN_TASK",

	[BTN_JOYSTICK]		= "BTN_JOYSTICK",
	[BTN_TRIGGER]		= "BTN_TRIGGER",
	[BTN_THUMB]		= "BTN_THUMB",
	[BTN_THUMB2]		= "BTN_THUMB2",
	[BTN_TOP]		= "BTN_TOP",
	[BTN_TOP2]		= "BTN_TOP2",
	[BTN_PINKIE]		= "BTN_PINKIE",
	[BTN_BASE]		= "BTN_BASE",
	[BTN_BASE2]		= "BTN_BASE2",
	[BTN_BASE3]		= "BTN_BASE3",
	[BTN_BASE4]		= "BTN_BASE4",
	[BTN_BASE5]		= "BTN_BASE5",
	[BTN_BASE6]		= "BTN_BASE6",
	[BTN_DEAD]		= "BTN_DEAD",

	[BTN_GAMEPAD]		= "BTN_GAMEPAD",
	[BTN_A]			= "BTN_A",
	[BTN_B]			= "BTN_B",
	[BTN_C]			= "BTN_C",
	[BTN_X]			= "BTN_X",
	[BTN_Y]			= "BTN_Y",
	[BTN_Z]			= "BTN_Z",
	[BTN_TL]		= "BTN_TL",
	[BTN_TR]		= "BTN_TR",
	[BTN_TL2]		= "BTN_TL2",
	[BTN_TR2]		= "BTN_TR2",
	[BTN_SELECT]		= "BTN_SELECT",
	[BTN_START]		= "BTN_START",
	[BTN_MODE]		= "BTN_MODE",
	[BTN_THUMBL]		= "BTN_THUMBL",
	[BTN_THUMBR]		= "BTN_THUMBR",

	[BTN_DIGI]		= "BTN_DIGI",
	[BTN_TOOL_PEN]		= "BTN_TOOL_PEN",
	[BTN_TOOL_RUBBER]	= "BTN_TOOL_RUBBER",
	[BTN_TOOL_BRUSH]	= "BTN_TOOL_BRUSH",
	[BTN_TOOL_PENCIL]	= "BTN_TOOL_PENCIL",
	[BTN_TOOL_AIRBRUSH]	= "BTN_TOOL_AIRBRUSH",
	[BTN_TOOL_FINGER]	= "BTN_TOOL_FINGER",
	[BTN_TOOL_MOUSE]	= "BTN_TOOL_MOUSE",
	[BTN_TOOL_LENS]		= "BTN_TOOL_LENS",
	[BTN_TOUCH]		= "BTN_TOUCH",
	[BTN_STYLUS]		= "BTN_STYLUS",
	[BTN_STYLUS2]		= "BTN_STYLUS2",
	[BTN_TOOL_DOUBLETAP]	= "BTN_TOOL_DOUBLETAP",
	[BTN_TOOL_TRIPLETAP]	= "BTN_TOOL_TRIPLETAP",

	[BTN_WHEEL]		= "BTN_WHEEL",
	[BTN_GEAR_DOWN]		= "BTN_GEAR_DOWN",
	[BTN_GEAR_UP]		= "BTN_GEAR_UP",

	[KEY_OK]		= "KEY_OK",
	[KEY_SELECT]		= "KEY_SELECT",
	[KEY_GOTO]		= "KEY_GOTO",
	[KEY_CLEAR]		= "KEY_CLEAR",
	[KEY_POWER2]		= "KEY_POWER2",
	[KEY_OPTION]		= "KEY_OPTION",
	[KEY_INFO]		= "KEY_INFO",
	[KEY_TIME]		= "KEY_TIME",
	[KEY_VENDOR]		= "KEY_VENDOR",
	[KEY_ARCHIVE]		= "KEY_ARCHIVE",
	[KEY_PROGRAM]		= "KEY_PROGRAM",
	[KEY_CHANNEL]		= "KEY_CHANNEL",
	[KEY_FAVORITES]		= "KEY_FAVORITES",
	[KEY_EPG]		= "KEY_EPG",
	[KEY_PVR]		= "KEY_PVR",
	[KEY_MHP]		= "KEY_MHP",
	[KEY_LANGUAGE]		= "KEY_LANGUAGE",
	[KEY_TITLE]		= "KEY_TITLE",
	[KEY_SUBTITLE]		= "KEY_SUBTITLE",
	[KEY_ANGLE]		= "KEY_ANGLE",
	[KEY_ZOOM]		= "KEY_ZOOM",
	[KEY_MODE]		= "KEY_MODE",
	[KEY_KEYBOARD]		= "KEY_KEYBOARD",
	[KEY_SCREEN]		= "KEY_SCREEN",
	[KEY_PC]		= "KEY_PC",
	[KEY_TV]		= "KEY_TV",
	[KEY_TV2]		= "KEY_TV2",
	[KEY_VCR]		= "KEY_VCR",
	[KEY_VCR2]		= "KEY_VCR2",
	[KEY_SAT]		= "KEY_SAT",
	[KEY_SAT2]		= "KEY_SAT2",
	[KEY_CD]		= "KEY_CD",
	[KEY_TAPE]		= "KEY_TAPE",
	[KEY_RADIO]		= "KEY_RADIO",
	[KEY_TUNER]		= "KEY_TUNER",
	[KEY_PLAYER]		= "KEY_PLAYER",
	[KEY_TEXT]		= "KEY_TEXT",
	[KEY_DVD]		= "KEY_DVD",
	[KEY_AUX]		= "KEY_AUX",
	[KEY_MP3]		= "KEY_MP3",
	[KEY_AUDIO]		= "KEY_AUDIO",
	[KEY_VIDEO]		= "KEY_VIDEO",
	[KEY_DIRECTORY]		= "KEY_DIRECTORY",
	[KEY_LIST]		= "KEY_LIST",
	[KEY_MEMO]		= "KEY_MEMO",
	[KEY_CALENDAR]		= "KEY_CALENDAR",
	[KEY_RED]		= "KEY_RED",
	[KEY_GREEN]		= "KEY_GREEN",
	[KEY_YELLOW]		= "KEY_YELLOW",
	[KEY_BLUE]		= "KEY_BLUE",
	[KEY_CHANNELUP]		= "KEY_CHANNELUP",
	[KEY_CHANNELDOWN]	= "KEY_CHANNELDOWN",
	[KEY_FIRST]		= "KEY_FIRST",
	[KEY_LAST]		= "KEY_LAST",
	[KEY_AB]		= "KEY_AB",
	[KEY_NEXT]		= "KEY_NEXT",
	[KEY_RESTART]		= "KEY_RESTART",
	[KEY_SLOW]		= "KEY_SLOW",
	[KEY_SHUFFLE]		= "KEY_SHUFFLE",
	[KEY_BREAK]		= "KEY_BREAK",
	[KEY_PREVIOUS]		= "KEY_PREVIOUS",
	[KEY_DIGITS]		= "KEY_DIGITS",
	[KEY_TEEN]		= "KEY_TEEN",
	[KEY_TWEN]		= "KEY_TWEN",
#ifdef KEY_VIDEOPHONE
	[KEY_VIDEOPHONE]	= "KEY_VIDEOPHONE",
#endif
#ifdef KEY_GAMES
	[KEY_GAMES]		= "KEY_GAMES",
#endif
#ifdef KEY_ZOOMIN
	[KEY_ZOOMIN]		= "KEY_ZOOMIN",
#endif
#ifdef KEY_ZOOMOUT
	[KEY_ZOOMOUT]		= "KEY_ZOOMOUT",
#endif
#ifdef KEY_ZOOMRESET
	[KEY_ZOOMRESET]		= "KEY_ZOOMRESET",
#endif
#ifdef KEY_WORDPROCESSOR
	[KEY_WORDPROCESSOR]	= "KEY_WORDPROCESSOR",
#endif
#ifdef KEY_WORDPROCESSOR
	[KEY_EDITOR]		= "KEY_EDITOR",
#endif
#ifdef KEY_SPREADSHEET
	[KEY_SPREADSHEET]	= "KEY_SPREADSHEET",
#endif
#ifdef KEY_GRAPHICSEDITOR
	[KEY_GRAPHICSEDITOR]	= "KEY_GRAPHICSEDITOR",
#endif
#ifdef KEY_PRESENTATION
	[KEY_PRESENTATION]	= "KEY_PRESENTATION",
#endif
#ifdef KEY_DATABASE
	[KEY_DATABASE]		= "KEY_DATABASE",
#endif
#ifdef KEY_NEWS
	[KEY_NEWS]		= "KEY_NEWS",
#endif
#ifdef KEY_VOICEMAIL
	[KEY_VOICEMAIL]		= "KEY_VOICEMAIL",
#endif
#ifdef KEY_ADDRESSBOOK
	[KEY_ADDRESSBOOK]	= "KEY_ADDRESSBOOK",
#endif
#ifdef KEY_MESSENGER
	[KEY_MESSENGER]		= "KEY_MESSENGER",
#endif
#ifdef KEY_DISPLAYTOGGLE
	[KEY_DISPLAYTOGGLE]	= "KEY_DISPLAYTOGGLE",
#endif

	[KEY_DEL_EOL]		= "KEY_DEL_EOL",
	[KEY_DEL_EOS]		= "KEY_DEL_EOS",
	[KEY_INS_LINE]		= "KEY_INS_LINE",
	[KEY_DEL_LINE]		= "KEY_DEL_LINE",

	[KEY_FN]		= "KEY_FN",
	[KEY_FN_ESC]		= "KEY_FN_ESC",
	[KEY_FN_F1]		= "KEY_FN_F1",
	[KEY_FN_F2]		= "KEY_FN_F2",
	[KEY_FN_F3]		= "KEY_FN_F3",
	[KEY_FN_F4]		= "KEY_FN_F4",
	[KEY_FN_F5]		= "KEY_FN_F5",
	[KEY_FN_F6]		= "KEY_FN_F6",
	[KEY_FN_F7]		= "KEY_FN_F7",
	[KEY_FN_F8]		= "KEY_FN_F8",
	[KEY_FN_F9]		= "KEY_FN_F9",
	[KEY_FN_F10]		= "KEY_FN_F10",
	[KEY_FN_F11]		= "KEY_FN_F11",
	[KEY_FN_F12]		= "KEY_FN_F12",
	[KEY_FN_1]		= "KEY_FN_1",
	[KEY_FN_2]		= "KEY_FN_2",
	[KEY_FN_D]		= "KEY_FN_D",
	[KEY_FN_E]		= "KEY_FN_E",
	[KEY_FN_F]		= "KEY_FN_F",
	[KEY_FN_S]		= "KEY_FN_S",
	[KEY_FN_B]		= "KEY_FN_B",

#ifdef KEY_BRL_DOT1
	[KEY_BRL_DOT1]		= "KEY_BRL_DOT1",
#endif
#ifdef KEY_BRL_DOT2
	[KEY_BRL_DOT2]		= "KEY_BRL_DOT2",
#endif
#ifdef KEY_BRL_DOT3
	[KEY_BRL_DOT3]		= "KEY_BRL_DOT3",
#endif
#ifdef KEY_BRL_DOT4
	[KEY_BRL_DOT4]		= "KEY_BRL_DOT4",
#endif
#ifdef KEY_BRL_DOT5
	[KEY_BRL_DOT5]		= "KEY_BRL_DOT5",
#endif
#ifdef KEY_BRL_DOT6
	[KEY_BRL_DOT6]		= "KEY_BRL_DOT6",
#endif
#ifdef KEY_BRL_DOT7
	[KEY_BRL_DOT7]		= "KEY_BRL_DOT7",
#endif
#ifdef KEY_BRL_DOT8
	[KEY_BRL_DOT8]		= "KEY_BRL_DOT8",
#endif
};
/* }}} */

/* Program code */
/* Private functions */
/* Returns whether the `sym' is being pressed according to `mask'.
 * The comparison ignores the case of `sym', and it does not have
 * to include the "KEY_" and "BTN_" prefix. */
static int match(unsigned char const *mask, char const *sym)
{
	unsigned i;

	/* TODO This linear search is quite ineffective. */
	for (i = 0; i < MEMBS_OF(Symbols); i++)
	{
		/* All symbolic names in `Symbols' are prefixed
		 * either with "KEY_" or "BTN_", so addressing
		 * the fourth character is safe. */
		if (!Symbols[i] || !BIT_OF(mask, i))
			continue;
		if (!strcasecmp(&Symbols[i][4], sym))
			return 1;
		if (!strcasecmp(&Symbols[i][0], sym))
			return 1;
	}

	return 0;
} /* match */

/* The main function */
int main(int argc, char const *argv[0])
{
	int hdev;
	char const *dev;
	unsigned char kbits[BITMAP(MEMBS_OF(Symbols))];

	/* Parse the "-d" command line argument.
	 * dev := path to the evdev device */
#ifdef __ARMEL__
	dev = "/dev/input/event2";
#else
	dev = "/dev/input/event0";
#endif
	if (argv[1] && !strcmp(argv[1], "-d"))
	{
		if (!argv[2])
		{
			fputs("getkey: Required argument missing.\n", stderr);
			exit(255);
		}

		dev = argv[2];
		argv += 2;
		argc -= 2;
	}

	/* Open the evdev interface.
	 * hdev := file descriptor of the evdev device */
	if ((hdev = open(dev, O_RDONLY)) < 0)
	{
		fprintf(stderr, "getkey: %s: %s\n",
			dev, strerror(errno));
		exit(255);
	}

	/* kbits := bitmask of all keys pressed at the moment */
	memset(kbits, 0, sizeof(kbits));
	if (ioctl(hdev, EVIOCGKEY(sizeof(kbits)), kbits) < 0)
	{
		fprintf(stderr, "getkey: %s: EVIOCGKEY: %s\n",
			dev, strerror(errno));
		exit(255);
	}

	/* Process the rest of the command line. */
	if (!argv[1])
	{
		unsigned i;

		/* Get mode: print the symbolic names of
		 * all keys currently pressed. */
		for (i = 0; i < MEMBS_OF(Symbols); i++)
			if (BIT_OF(kbits, i))
				printf("%s 0x%.4X %u\n",
					Symbols[i]
						? Symbols[i]
						: "<unknown>",
					i, i);
		exit(0);
	} else
	{
		int allok, checkany;
		unsigned i, exitcode;

		/* Test mode: test whether all key specified
		 * on the command line block are pressed and
		 * exit with the appropriate code. */
		allok = 1;
		checkany = 0;
		exitcode = 0;
		for (i = 1; ; i++)
		{
			if (!argv[i] || !strcmp(argv[i], "-o"))
			{
				/* End of command line block */
				if (checkany && allok)
					/* All of the keys in the block
					 * are being pushed. */
					break;
				exitcode++;
				if (!argv[i])
					/* Out of blocks */
					break;

				/* Process the next block. */
				allok = 1;
				checkany = 0;
			} else
			{
				/* Test wheter argv[i] is pressed
				 * and reset `allok' if it isn't.
				 * There's no point in testing if
				 * the block has already failed. */
				checkany = 1;
				if (allok && !match(kbits, argv[i]))
					allok = 0;
			} /* if */
		} /* for */
		exit(exitcode);
	} /* if */

	/* Not reached */
} /* main */

/* vim: set foldmethod=marker: */
/* End of getkey.c */
