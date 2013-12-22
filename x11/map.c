#ifdef i_am_a_shell_program /* compile ourselves {{{ */
  : Our only requirement is x11.  Let us see which optional packages
  : are available.
  pkgs="x11";
  optional="xfixes xext xcomposite xtst xi xres xft";

  : We can live without pkg-config if we must, but we will be very limited.
  if pkg-config --version > /dev/null 2>&1;
  then
    have_pkgconf="yes";

    : Verify that required $pkgs are installed.
    if ! pkg-config $pkgs;
    then
      echo "$pkgs are required" >&2;
      exit 1;
    fi

    for pkg in $optional;
    do
      : Leave xinput out if not building with xtst,
      : because we would not use it.
      [ "$pkg" = "xi" -a "$with_xtst" = "no" ] \
        && continue;

      define="$pkg";
      eval with=\$with_$define;
      [ "$with" = no ]  && continue;
      pkg-config "$pkg" || continue;

      pkgs="$pkgs $pkg";
      defines="$defines $define";
      eval with_$define="yes";
    done
  else
    if [ ! -d /usr/include/X11 ];
    then
      echo "libx11-dev is required" >&2;
      exit 1;
    fi

    echo "Warning: pkg-config not found, cannot test for optional features" \
      >&2;
    have_pkgconf="no";
  fi

  : These are safe to enable every time autodetected
  : because they do not depend on libraries.
  [ -f /usr/include/linux/fb.h ] \
    && defines="$defines fb";
  [ -f /usr/include/linux/omapfb.h ] \
    && defines="$defines omapfb";
  if [ -f /usr/include/SGX/hwdefs/sgx530defs.h ];
  then
    : We only need some constants from the headers.
    defines="$defines sgx";
    CFLAGS="$CFLAGS -I/usr/include/SGX/hwdefs";
    CFLAGS="$CFLAGS -I/usr/include/SGX/include4";
  fi
  if [ -f /targets/links/scratchbox.config ];
  then
    : Check for Fremantle.
    source /targets/links/scratchbox.config;
    [ "$SBOX_CROSS_GCC_NAME" = "cs2007q3-glibc2.5-arm7" ] \
      && defines="$defines fremantle";
  fi

  : We can take advantage of XRes 1.2, but unfortunately the package
  : version does not reflect the extension version in Harmattan.
  [ "$with_xres" = "yes" ] \
    && grep -wq XResQueryClientIds /usr/include/X11/extensions/XRes.h \
    && defines="$defines xres_12";

  : Check for xi2.
  [ "$with_xi" = "yes" -a -f /usr/include/X11/extensions/XInput2.h ] \
    && defines="$defines xi2";

  : Basically, we are a C program.
  lang="-x c";

  : Decide which toolkit to link with.  Prefer C, as it compiles faster.
  if [ "$have_pkgconf" = "yes" ];
  then
    [ "$with_qt" != "no" ] && pkg-config QtGui \
      || with_qt="no";
    [ "$with_gdk_pixbuf" != "no" ] && pkg-config gdk-pixbuf-2.0 \
      || with_gdk_pixbuf="no";
    if [ "$with_gdk_pixbuf" != "no" -a "$with_qt" != "yes" ];
    then
      pkgs="$pkgs gdk-pixbuf-2.0";
      defines="$defines gdk_pixbuf";
    elif [ "$with_qt" != "no" ]
    then
      pkgs="$pkgs QtGui";
      defines="$defines qt";
      : ${CC:="c++"};
      lang="-x c++";
    fi
  fi

  : ${CC:="cc"};
  for define in `echo "$defines" | tr a-z A-Z`;
  do
    CFLAGS="$CFLAGS -DHAVE_$define";
  done

  if [ "x$1" = "x-v" ];
  then
    echo="echo";
    shift;
  else
    echo="";
  fi

  if [ "$defines" != "" ];
  then
    echo -n "Building with";
    CFLAGS="$CFLAGS -DCONFIG_FEATURES=";
    [ "$echo" ] && CFLAGS="${CFLAGS}'";
    for define in $defines;
    do
      echo -n " $define";
      CFLAGS="${CFLAGS}FEATURE(\"$define\")";
    done
    CFLAGS="${CFLAGS}";
    [ "$echo" ] && CFLAGS="${CFLAGS}'";
    echo .;
  fi

  : We need sqrt to calculate Eucledian distances.
  if [ "$have_pkgconf" = "yes" ];
  then
    CFLAGS="$CFLAGS `pkg-config --cflags $pkgs`";
    LIBS="-lm `pkg-config --libs $pkgs`";
  else
    LIBS="-lm -lX11";
  fi
  echo "This program is free software.  It does whatever it wants.";

  : Build or simply echo the compilation command line.
  [ "$echo" -o -f "$0" ] \
    && exec $echo $CC -Wall $CFLAGS $lang "$0" $LIBS "$@";

  : Otherwise the source code is coming from the standard input,
  : for example from wget.  Get it from the heredoc and compile that.
  {
   : Eat the #endif.  We have to run the rest through cat,
   : because it is possible that the shell wrote the heredoc
   : to a temporary file, and cpp seems to rewind files.
   while read line rest && [ "$line" != "#endif" ]; do :; done
   cat | $CC -Wall $CFLAGS -include /dev/stdin $lang /dev/null $LIBS "$@"; exit;
  } << 'END_OF_PROGRAM';
#endif /* i_am_a_shell_program }}} */
/*
 * map.c -- X window manipulator {{{
 *
 * This program allows you to execute various primitive X commands
 * on a set of windows.
 *
 * Dedicated to Kimmo Ha:ma:la:inen for his expertise and patience.
 *
 * You can compile it either by:
 * $ [with_<package>="no"]... sh map.c [-v] [<compiler-flags>] -o map
 *   # Autodetects what you have, but you can selectively disable <package>s.
 *   # This is useful if the running environment will be more constrained
 *   # than the compilation environment.  With the -v flag you can see
 *   # the compiler command line without executing it.
 * $ cc -Wall [<compiler-flags>] map.c -lX11 -lm -o map
 *   # To build a bare-bones version.
 *
 * Usage: ./map {[-W do] <command>... [<window>...] [-W <repeat>]}...
 *
 * Available commands are:
 * -v                   Increase the verbosity level.  If it's the only
 *                      option we just print the choosen <window>s XID.
 * -q                   Print some information about the <window>s:
 *                      geometry, event masks, shapes.  Prefixed with -Q
 *                      the entire subtrees of <window>s are printed as well.
 * -r                   Print the resources of the client which owns <window>.
 *                      At increased verbosity levels it prints more resource
 *                      types.  Prefixed with -Q it displays all clients'
 *                      resource usage.  With -QQ sums up all the resources
 *                      used in X.
 * -z <output>          Save a screenshot of <window>'s contents.  The format
 *                      of the image is determined by <output>'s extension;
 *                      if unknown, it will be a simple <RGB><RGB><RGB>...,
 *                      one byte per color channel.  If output is enclosed
 *                      in //-es (eg. "/foo.png/", a normally invalid file
 *                      name) you can use %c-like tokens: %Y, %M, %D to add
 *                      the current date, %h, %m, %s to add the current time,
 *                      or %c to add a counter.  See below the details.
 * -Qz <output>         Likewise, but dump the current contents of the
 *                      frame buffer.  <window>, as a number, designates
 *                      which fb to dump, the default being fb0.
 * -QQz <output>        This is an obscure sgx-specific feature.  It can be
 *                      used to dump frames of software renderings, which
 *                      are transmitted between the clients and the server
 *                      through shared memory.  This command attempts to
 *                      find the latest such segment used by the owner of
 *                      <window>, and assuming that it was actually used
 *                      to draw that <window>, deduces the image details
 *                      and saves it.  If <window> is the root window,
 *                      the latest rendered fullscreen frame is dumped.
 * -QQQz <output>       Likewise, but rather than just the latest one,
 *                      all frames thought to be belonging to <window>
 *                      are dumped.  You should specify a template as
 *                      <output> otherwise the dumps will overwrite each
 *                      other.
 * -nN [!][OR=]<geo>[@{none|input|<color>}]
 *                      Create a new window with the specified geometry and
 *                      background color.  -n also maps it, but with -N you
 *                      can defer mapping until you do it explicitly with -m;
 *                      This is useful if you want to set initial properties
 *                      and state.  The new window's parent will be <window>,
 *                      or the root window.  "none" in place of the <color>
 *                      indicates that the contents of the parent window be
 *                      inherited.  You can create a 32-bit window by giving
 *                      an <alpha> value in <color> (full specs are below).
 *                      Specifying "input" as color creates an InputOnly
 *                      window, and the "OR=" prefix makes it initially
 *                      override-redirected.  The new window's name will be
 *                      "map_<n>_<pid>", where <n> is a monotonic counter,
 *                      so you can refer to this window as "map_<n>" or
 *                      simply "map" (if it's unambiguous) or as "new"
 *                      (until you create another widnow).  You can change
 *                      the name and override-redirected status with -o.
 *                      Unless overridden by the '!' prefix the subsequent
 *                      commands will operate on the newly created window.
 * -g <geo>             Move and/or resize the specified <window>s.
 * -g {shape|clip|input}={none|clear|[~]<geo>[{,|&|/}[~]<geo>]...}
 *                      Set the bounding or the input shape of the <window>s.
 *                      Subsequent rectangles specified by <geo>s can be
 *                      united if they are separated by a comma or '|',
 *                      or intersected ('&') or subtracted from each other
 *                      ('/').  By prefixing with '~' the inverse of the
 *                      <geo> is taken.  "none" (or "clear") restores the
 *                      window's natural shape.
 * -l                   List the <window>s properties.
 * -iIwas <key>         Prints the current value of an INTEGER, CARDINAL,
 *                      WINDOW, ATOM or STRING property of <window>.
 *                      Special <key>s are recognized:
 *                        "support"       -> "_HILDON_PORTRAIT_MODE_SUPPORT"
 *                        "request"       -> "_HILDON_PORTRAIT_MODE_REQUEST"
 *                        "noncomp", "nc" -> "_HILDON_NON_COMPOSITED_WINDOW"
 *                        "dnd"           -> "_HILDON_DO_NOT_DISTURB"
 *                        "dnd_override"  -> "_HILDON_DO_NOT_DISTURB_OVERRIDE"
 * -x <key>             Remove the <key> property from the <window>s.
 * -s <key>=<val>       Sets or changes the value of <key>, a STRING property.
 * -iIwa <key>={<integer>|<atom>},...
 *                      Sets or changes the value of an INTEGER, CARDINAL,
 *                      WINDOW or ATOM property.  Multiple values can be
 *                      specified.  Atom names are automatically internalized.
 * -p append|prepend    Optional prefix signifying that the following property
 *                      value should be appended or prepended to the existing
 *                      values.  Could be useful in creating property arrays.
 * -p toggle            This is an optional prefix of the -iIwas <key>=<val>
 *                      command, which toggles <key> if it <window> has such
 *                      property: if its current value is zero (0, None or
 *                      empty string) the it's changed to <val>, otherwise
 *                      it's changed to zero.
 * -p flip              Another optional prefix like -t.  If <window> doesn't
 *                      have a <key> property it's set to <val>, otherwise
 *                      it's deleted.
 * -f {parent|root|none} Set the input focus to <window> and make it revert
 *                      to the parent/root window or to nowhere when <window>
 *                      is unmapped.
 * -C [win=<event-window>] <msg>[=<param>,...]
 *                      Send a 32-bit format ClientMessage to <window>.
 *                      Unless the optional <event-window> is specified
 *                      <window> will be the XEvent::window too.  <param>s
 *                      can either be integers or Atom names.
 * -E [win=<event-window>] obscured|unobscured
 * -E [win=<event-window>] visibility={obscured|partial|unobscured}
 *                      Synthetise an event notifying <window> about the
 *                      visibility of <event-window>.
 * -E [win=<event-window>] {newprop|propdel}=<key>
 *                      Synthetise an XPropertyEvent.
 * -A show=<visible>,<opacity>
 * -A move=<x>,<y>,<depth>
 * -A anchor=<gravity>,<x>,<y>
 * -A rotate=<axis>,<degrees>,<x>,<y>,<z>
 * -A scale=<scale-x>,<scale-y>
 *                      Move, rotate, scale, etc. a hildon animation actor.
 *                      All parameters are optional, and if missing ones are
 *                      substituted with a reasonable default.  With <visible>
 *                      you can hide (0) or show (1) an actor.  <opacity> is
 *                      a number between 0..255.  <x>, <y>, and <depth> are
 *                      coordinates.  <gravity> is a number 0..9 (none, north,
 *                      north-east, east, ...).  Unless it's 0 (none) <x> and
 *                      <y> are ignored.  <axis> is a number designating the
 *                      [xyz] axes.  The coordinates specify the center of the
 *                      rotation.  <scale-x> and <scale-y> are percentages.
 *                      See the examples.  (Specific to hildon-desktop.)
 * -o name=<something>  Make the <window>'s WM_NAME <something>.
 * -o !name             Unset the WM_NAME of <window>.
 * -o [!]OR             Make the <window>s override redirected or clear
 *                      this attribute.
 * -o [!]focusable      Accept (default) or reject keyboard focus.
 * -o [!]starticonic    Make the <window>s initially iconic when mapped.
 * -o [!]{normal|iconic|withdrawn}
 *                      Change the <window>'s WM_STATE to normal/iconic
 *                      or withdrawn/normal.  This interferes with the
 *                      window manager, which may not handle the case
 *                      gracefully.
 * -o [!]fs             Fullscreen/unfullscreen the window.  This is also
 *                      for making a window initially fullscreen.
 * -mu                  Map or unmap the specified <window>s.
 * -RL [<sibling>]      Raise the specified <windows> just above or below
 *                      <sibling>, or to the top or bottom if <sibling> is
 *                      the root window.  (Doesn't work with hildon-desktop.)
 * -L lo|bottom         Lower <window> to the bottom of the stack.
 * -R hi|lo|bottom      Raise it to the top or to the bottom of the stack.
 * -dDK                 Either ask the window to close by seding WM_DELETE
 *                      or destroy it or kill the client altogether.
 * -k [[<duration>]:][{shift|ctrl|alt|fn}-]...{<keysym>|<string>}
 *                      Simulate a key press of $duration miliseconds,
 *                      100 by default.  X11/keysymdef.h is a source
 *                      of possible <keysym>s.  Note that <keysym>s
 *                      are case sensitive.  <window> is ignored.
 * -c <xpos>[@<click-time>|!]
 *                      Simulate a left-click at <xpos>.  By specifying
 *                      a <window> you can control a background window.
 *                      <click-time> tells the desired duration of the
 *                      click in ms, which is 250ms by default.  '!' tells
 *                      not to release.  See below for the specification
 *                      of <xpos> and <time>s.
 * -c <start-xpos>[@<time-to-press>][[,<move-time>],<xpos>]*,<final-xpos>[@<time-to-release>|!]
 *                      Simulate a series of pointer movements from
 *                      <start-pos> through intermediate <xpos>es until
 *                      <final-xpos> through straight lines, whose points
 *                      are interpolated.  The pointer moves with an
 *                      exponentially growing speed on each section,
 *                      and takes the distances in <move-time>s (950ms
 *                      by default when travelling between far edges).
 *                      <time-to-press> and <time-to-release> are the
 *                      <time>s in ms to wait since the last movement
 *                      before the press/release.  By default there's
 *                      no wait.
 * -c {left|right|up|down}
 *                      Simulate panning from the left/right/top/bottom
 *                      edge of the screen.
 * -c {swipe|swleft|swright|swup|swdown}
 *                      Swipe left/right/up/down.  Plainly "swipe" swipes
 *                      to the left.
 * -c qlb               Open the quick launchbar.
 * -G fill=<geo>[<color>]
 * -G text=<X>x<Y>[<color>],<text>[,<font>]
 *                      Draw on the window (or pixmap or other drawable).
 * -X [u]app[{@none|<color>}]
 *                      Creates a simple maximized application window.
 *                      If prefixed with 'u' you'll have to map it explicitly.
 * -X [u]mapp[{@none|<color>}]
 *                      Likewise, but makes it undecorated.
 * -X [u]desktop[{@none|<color>}]
 *                      Creates a desktop window.
 * -X top               Present the <window> as the applications does.
 * -X iconify           Iconify the <window> or the topmost one.
 * -X close             Request the application that <window> (or the
 *                      topmost one) be closed.
 * -X tasw              Go to the task switcher.  <window> is ignored.
 * -X {fullscreen|fs}   Toggle fullscreenness.  Only works on managed windows.
 * -X ping              Ping the <window> or the topmost one.
 * -X rotate            Rotate the screen forcefully but don't touch
 *                      any flags.  (hildon-desktop-specific)
 * -X portrait          Toggle portrait mode of <window>s or the current
 *                      application. (likewise)
 * -X {noncomp|nc}      Toggle composition. (and likesise)
 * -W <time>            Wait for <time> and don't do anything to <window>.
 *                      <time> is interpreted as seconds by default; see
 *                      below for the full details.  If <time> is 0, wait
 *                      until a keypress.
 * -W do                Mark the beginning of the command block, from which
 *                      command repetition will start.
 * -W repeat[=<N>]      Repeat the command block <N> times or infinitely.
 * -W loop[=<N>]        Repeat the commands from the beginning <N> times
 *                      infinitely.
 * -W quit              Normally, if you've created windows you're prompted
 *                      before quitting.  This command makes us quit without
 *                      prompting.
 * -W exit              When quitting, leave the windows we created around.
 *
 * <time> expressions (eg. 250ms, 0.3) are integer or fractional numbers
 * optionally followed by "s" or "ms" to designate the scale.  If it is
 * unspecified and the number is fractional, it is taken as seconds,
 * otherwise its meaning is context-dependant.
 *
 * An <xpos> is a co-ordinate specification like 100x200.  You can use
 * the shorthands tl, bl, tr and br for the corners.  A co-ordinate
 * can be given relative to the screen size like 0.5x0.5.  This case
 * you can append an offset too: 0.5-10x0.5+10.  Co-ordinates and
 * offsets can be in pixels (default), milimeters, centimeters and
 * inches (though only pixels are reliable).  You can change the origin
 * by appending a corner name: 10x10br is the point ten pixels far
 * from the bottom and right edges towards the left and bottom edges.
 * The default origin is top-left and the gravity is always opposite
 * to the origin (ie. bottom-right by default).
 *
 * A <geo> is like a geometry in standard X notation (WxH+X+Y), but more
 * flexible.  You can apply all the extensions described above with a few
 * differences: WxH does not accept an origin (since it's meaningless),
 * but does understand negative sizes (eg. -100x-100) meaning that many
 * pixels shorter.  You can use "fs" to designate full-screen size.
 * If +X+Y is omitted, the top-left origin is assumed.
 *
 * <color>s are usually prefixed by '@' followed by a color name or direct
 * pixel value.  "random" as a color name will choose a random color.
 * You can add an alpha channel by postfixing it with '%' and the numeric
 * value of the alpha.
 *
 * <window> designates the window you want to manipulate.  It can be any
 * of the following:
 *   -- an XID (decimal and hexadecimal numbers are accepted).  You can
 *      determine the XID of the window of your interest with xwinifo(1).
 *   -- XID=0 or "root": the root window of the default screen.
 *   -- "overlay" means the composition overlay window
 *   -- "wm" selects the _NET_SUPPORTING_WM_CHECK window
 *   -- "new" refers to the latest window created by us; if we haven't
 *      created any it simply refers to the root window
 *   -- a window name or class hint.  Unless the name is prefixed with '!'
 *      it is taken as a substring and matched case insensitively.
 *   -- "top": selects the currently active window (focused or otherwise)
 *      according to your window manager.  This only selects application
 *      windows.
 *   -- "top-SOMETHING" finds the topmost SOMETHING, where SOMETHING is
 *      checked against the _NET_WM_WINDOW_TYPE of clients and can be APP,
 *      DIALOG or anything else.
 *   -- "select": `map' will let you pick the window by pointing to it,
 *      like xwininfo(1) does when started without XID.  Left click selects
 *      the client window, while right click selects the frame window that
 *      the window manager created for the client.  Holding down <shift>
 *      and left clicking selects the subwindow right below the pointer,
 *      which is usually a widget.  Finally, the middle button cancels
 *      the selection and moves on to the subsequent <window>.
 *
 * The command line is partitioned into command blocks: a list of commands
 * and windows they operate on.  The beginning of a command block can be
 * explicitly indicated with -W do.  In a command block all commands are
 * executed on each window before moving on to the next window.  When done,
 * command blocks can be repeated with -W repeat or -W loop.
 *
 * Commands can operate without <window>s.  Some of them don't need them
 * at all (eg. -X tasw), others will implicitly take the root window
 * (eg. -n), a few others do the same with the topmost one (eg. -X ping).
 * Some commands, because of their nature, require an explicit <window>
 * (eg. -m).
 *
 * Examples:
 *   * map -u select
 *     Unmap the tapped client window.
 *   * map -m rtcom-call-ui
 *     Remap Phone's topmost window.
 *   * map -k ctrl-BackSpace
 *   * map -X tasw
 *     Go to task switcher.
 *   * map -C win=top _NET_WM_STATE=2,_NET_WM_STATE_FULLSCREEN
 *   * map -X fs
 *     Fullscreen or unfullscreen the current application.
 *   * map -C win=top _NET_CLOSE_WINDOW
 *   * map -X close
 *     Close the current application.
 *   * map -v top-DIALOG top-STATUS_AREA
 *     Print the XID of the topmost dialog and the status area.
 *   * map -N 100x100 -o fs -m 0
 *     Create a new 100x100 non-override-redirected, fullscreen window,
 *     which is a subwindow of the root window.
 *   * map -g shape=~.5x.5+.25+.25/1.0x.5+.0+.25 top
 *     Cut a strip from the middle of the topmost window.
 *   * map -g shape=.25x.25tl,.25x.25tr,.25x.25bl,.25x.25br top
 *     Only keep the four corners of the window.
 *   * map -Qq
 *     Like xwininfo -root -children but see all information instantly.
 *   * map -N 10x20 -s akarmi=valami -W loop=2
 *     Create a three level window hierarchy.
 *   * map -X mapp -W do -X tasw -W 0 -W loop
 *     Create an application window, then go to the switcher, wait,
 *     Create an application window, then go to the switcher, wait, ...
 *   * map -X mapp -W do -X tasw -W 2s -X top new -W 2s -W repeat
 *     Create an application window once, then go to the switcher, wait,
 *     Create an application window, then go to the switcher, wait, ...
 *   * map -z /ide_%t_%20C.png/ -W 5s -W loop
 *     Take a screenshot every five seconds and preserve the last 20 of them.
 *   * map -c cc
 *     Click the middle of the screen (center-center).
 *   * map -c 400x400,400x300
 *   * map -c 400x300,400x400
 *     Pan.
 *   * map -umu top
 *     Fun!
 *
 * Hildon-specific examples:
 *   * map -tI request=1 -W loop
 *     On tap switch to portrait mode and back.
 *   * map -Ti nc=1 top
 *   * map -X nc
 *     Make the current application composited/non-composited.
 *   * map -c 56,28 0
 *     Another way of entering/leaving the task switcher/launcher.
 *   * map -C win=Osso_calculator _NET_ACTIVE_WINDOW
 *   * map -X top Osso_calculator
 *     Make Calculator the current application.
 *   * map -c 56,38 -W 1.5 -c 246,366 -W 1.5 -c 744,44
 *     Provided that you're on the Home screen launch Calculator then close it
 *     as if you were doing it.
 *   * map -A show=1,127 top-ACTOR
 *     Make the topmost hildon animation actor half-transparent.
 *   * map -A move=0,0,-500 top-ACTOR
 *     Move it away from the camera.
 *   * map -A anchor=9 top-ACTOR
 *     Anchor it at the center of the actor (does not change its position).
 *   * map -A rotate=2,45,400,240
 *     Rotate the actor by 45 degrees around (400,240).
 *   * map -A scale=150,75
 *     Make it 50% wider and 25% shorter.
 *
 * Tokens recognized in <output>s are:
 *   * %Y:      four-or-more-digit year
 *   * %M:      two-digit month of the year
 *   * %D:      two-digit day of the month
 *   * %h:      two-digit hour of the day
 *   * %m:      two-digit minutes of the hour
 *   * %s:      two-digit seconds of the minute
 *   * %u:      six-digit microseconds of the second
 *   * %t:      shorthand for "%Y-%M-%D_%h:%m:%s"
 *   * %S:      Unix time
 *   * %T:      shorthand for "%S.%u"
 *   * %[<n>]c: the current value of an incrementing counter,
 *              zero-padded to be at least <n>-character wide
 *   * %<n>C:   the current value of the same counter, except that
 *              it is reset once <n> is reached; practicelly,
 *              a rolling counter, padded with zeroes so that
 *              all values are as wide as the counter's maximum
 * Note that all %c and %C tokens share the same counter.
 *
 * Full specifications of the grammar of <color>, <xpos> and <geo>:
 *
 * pixel      := <integer>
 * alpha      := <integer>
 * color-name := <pixel> | <xcolor-name> | rnd | rand | random
 * color      := @<color-name>[%<alpha>] | %<alpha> | #<alpha>[@<color-name>]
 *
 * unit   := px|mm|cm|in
 * off    := <integer>[<unit>]
 *        # offset in pixels
 * rel    := <non-zero float>
 *        # position or dimension relative to the full width/height
 * dim    := <unsigned>[<unit>] | <negative>[<unit>] | {<rel>[<off>]}
 *        # relative/absolute dimension or dimension from full width/height
 *        # (equivalent to "1.0-<off>")
 *
 * coord  := <off> | {<rel>[<off>]}
 *        # eg. "10", "-10", "0.5", "0.5+10", "0.5-10"
 * origin := { "t" | "c" | "b" } { "l" | "c" | "r" }
 *        # "tl": top-left    ("0x0"),    "tr": top-right    ("0x1.0")
 *        # "cl": center-left ("0x0.5"),  "cr": center-right ("1.0x0.5")
 *        # "bl": bottom-left ("1.0x0"),  "br": bottom-right ("1.0x1.0")
 *        # the default origin is "tl"
 * corner := <origin>
 * xpos   := <corner> | {<coord> 'x' <coord> [<origin>]}
 *        # eg. "10x10tr" is "10x1.0-10"
 * pos    := <corner> | {<coord> <coord> [<origin>]}
 *        # eg. "+10+10", "+0.1+10+10", "+0.1-10+10"
 *
 * size   := "fs" | {<dim> 'x' <dim>}
 *        # "fs": fullscreen, ".5x.5": quarter of the screen,
 *        # "1.0x0.5-20": full-width and 20px shorter than half of the height,
 *        # "-20x-20": 20px shorter and narrower than the full screen
 * geo    := <size> [<pos>]
 *        # if <pos> is omitted, it's taken as +0+0
 *        # eg. ".5x.5+.25+.25": a centered square,
 *        # "20x20-20-20": a 20x20 rectangle in (-20, -20)
 *        # "20x20+1.0-20+1.0-20": a 20x20 rectangle in the top-right corner
 * }}}
 */

/* For strcasestr() */
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

/* We have some non-debug code in assert(). */
#undef NDEBUG

/* Include files */
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <getopt.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/ioctl.h>

#ifdef HAVE_FB
# include <linux/fb.h>
#endif
#ifdef HAVE_OMAPFB
# include <linux/omapfb.h>
#endif

/* Qt headers must be #include:d before X. */
#ifdef HAVE_QT
# include <QColor>
# include <QImage>
# include <QImageWriter>
#endif

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>

#ifdef HAVE_XFIXES
# include <X11/extensions/Xfixes.h>
#endif
#ifdef HAVE_XEXT
# include <X11/extensions/shape.h>
#endif
#ifdef HAVE_XCOMPOSITE
# include <X11/extensions/Xcomposite.h>
#endif
#ifdef HAVE_XTST
# include <X11/extensions/XTest.h>
#endif
#ifdef HAVE_XI
# include <X11/extensions/XInput.h>
#endif
#ifdef HAVE_XI2
# include <X11/extensions/XInput2.h>
#endif
#ifdef HAVE_XRES
# include <X11/extensions/XRes.h>
#endif
#ifdef HAVE_XFT
# include <X11/Xft/Xft.h>
#endif

#ifdef HAVE_GDK_PIXBUF
# include <gdk-pixbuf/gdk-pixbuf.h>
#endif

#ifdef HAVE_SGX
# define SGX530
# define SGX_CORE_REV 125
# ifndef LINUX
#  define LINUX
# endif
# include "sgxdefs.h"
#endif

/* Some symbols of xlib are different in C++. */
#ifndef __cplusplus
# define c_class                class
#endif

/* Macros */
/* Is $typeatom a string of characters? */
#define IS_STRING(typeatom)     \
        ((typeatom) == XA_STRING || (typeatom) == Utf8)

/* The number of elements of $ary. */
#define MEMBS_OF(ary)           ((int)(sizeof(ary) / sizeof((ary)[0])))

/* Type definitions */
/* How to set the next property? */
enum propcmds
{
  NONE,     /* Just replace the existing value.                             */
  PREPEND,  /* Prepend to the existing value(s).                            */
  APPEND,   /* Append to the existing value(s).                             */
  TOGGLE,   /* Toggle between 0 and 1.                                      */
  FLIP,     /* If the property doesn't exist, set it, otherwise remove it.  */
};

/* Whose clients' resources to print with -r? */
enum resource_listing_t
{
  SINGLE_CLIENT,  /* The one's specified on the command line.               */
  EACH_CLIENT,    /* Enabled by -Q, print each clients' separately.         */
  SYSTEMWIDE      /* Enabled by -QQ, print each clients' cumulatively.      */
};

/* Private constants */
/* The list of recognized options for getopt().  Unavailable letters are:
 * Aa Cc Dd E(e) Gg Ii Kk Ll Nn Qq Rr Ww Xx fmopsuvz.  Available letters:
 * Bb F Hh Jj M O P S Tt U V Yy Z. */
static char const *Optstring =
    "-vQqrz:n:N:g:lp:i:I:w:a:s:x:f:C:E:A:o:muR:L:dDKk:c:G:X:W:";

/* Private variables */
/* For X error trapping */
static unsigned char Last_xerror;
static int (*Orig_xerror_handler)(Display *, XErrorEvent *);

/* What we operate on */
static int Scr;
static Window Root;
static Display *Dpy;
static unsigned DpyWidth, DpyHeight;

/* Atom value of UTF8_STRING. */
static Atom Utf8;

/* The XID of the last window we created or $Root, for choose_window().
 * $NWindows is the number of windows we've created. */
static Window Newborn;
static unsigned NWindows;

/* Are the screen contents rotated counter-clockwise? */
static Bool Rotated;

/* $Verbose is the verbosity level.  If $Is_interacetive be a little
 * more friendly by adding more freely-formatted information. */
static int Verbose, Is_interactive;

/* Program code */
/* Private functions */
static void __attribute__((noreturn)) die(char const *msg)
{
    fputs(msg, stderr);
    exit(1);
} /* die */

#ifndef __cplusplus
static void *xmalloc(void *ptrp, size_t size)
{
  void *ptr;
  if (!(ptr = malloc(size)))
    die("Out of memory\n");
  return *(void **)ptrp = ptr;
} /* xmalloc */

static void *xrealloc(void *ptrp, size_t size)
{
  void *ptr;
  if (!(ptr = realloc(*(void **)ptrp, size)))
    die("Out of memory\n");
  memcpy(ptrp, &ptr, sizeof(ptr));
  return ptr;
} /* xrealloc */
#else /* __cplusplus */
template<typename T>
static T xmalloc(T *ptrp, unsigned size)
{
  if (!(*ptrp = (T)malloc(size)))
    die("Out of memory\n");
  return *ptrp;
} /* xmalloc */

template<typename T>
static T xrealloc(T *ptrp, unsigned size)
{
  if (!(*ptrp = (T)realloc(*ptrp, size)))
    die("Out of memory\n");
  return *ptrp;
} /* xrealloc */
#endif /* __cplusplus */

static char *mkstr(char const *str, size_t lstr)
{
  char *dup;

  xmalloc(&dup, lstr+1);
  memcpy(dup, str, lstr);
  dup[lstr] = '\0';

  return dup;
} /* mkstr */

/* Adds $str to an internal buffer, aborts on OOM.  If $str is NULL
 * resets the buffer.  Returns the start of the current buffer. */
static char const *addstr(char const *str)
{
  static char *buf;
  static size_t sbuf, lbuf;
  size_t lstr;

  if (str)
    lstr = strlen(str);
  else
    lstr = lbuf = 0;

  /* Rellocate the buffer if it's not large enough. */
  if (lstr+1 > sbuf - lbuf)
    {
      sbuf = lbuf + lstr + 1;
      xrealloc(&buf, sbuf);
    }

  /* Append $str and NUL-terminate. */
  if (str)
    memcpy(&buf[lbuf], str, lstr);
  lbuf += lstr;
  buf[lbuf] = '\0';

  return buf;
} /* addstr */

/* Returns whether $str is prefixed by $pfx, and if so, the substring
 * beginning after that. */
static char const *isprefix(char const *str, char const *pfx)
{
  while (*pfx)
    if (*str++ != *pfx++)
      return NULL;
  return str;
} /* isprefix */

/* MIN(MAX($n, $min), $max) */
static __attribute__((const))
int clamp(int n, int min, int max)
{
  if (n < min)
    return min;
  if (n > max)
    return max;
  return n;
} /* clamp */

static size_t __attribute__((unused))
larger(size_t lhs, size_t rhs)
{
  return lhs > rhs ? lhs : rhs;
} /* larger */

static unsigned __attribute__((unused))
roundto(unsigned n, unsigned alignment)
{
  unsigned m;

  m = n % alignment;
  return m ? n + alignment-m : n;
} /* roundto */

/* Store asynchronous X errors in $Last_xerror. */
static int xerror(Display *dpy, XErrorEvent *event)
{
  Last_xerror = event->error_code;
  return 0;
} /* xerror */

/* Ignore X errors. */
static void trap_xerrors(void)
{
  Orig_xerror_handler = XSetErrorHandler(xerror);
  Last_xerror = Success;
} /* trap_xerrors */

/* Return the last asynchronous X error. */
static unsigned char untrap_xerrors(void)
{
  XSync(Dpy, False);
  XSetErrorHandler(Orig_xerror_handler);
  return Last_xerror;
} /* untrap_xerrors */

/* Find out the primary output device's physical dimensions in milimeters.
 * Returns whether the results are thought to be accurate. */
static Bool get_dimensions(float *wmmp, float *hmmp)
{
#if defined(HAVE_FB) || defined(HAVE_OMAPFB)
  static int tried;
  static float wmm, hmm;
  int hfb;

  /* Ask the kernel because X falsifies the information such that
   * the DPI is some conventional value. */
  if (!tried && (hfb = open("/dev/fb0", 0)) >= 0)
    {
# ifdef HAVE_OMAPFB
      struct omapfb_display_info di;

      if (ioctl(hfb, OMAPFB_GET_DISPLAY_INFO, &di) == 0)
        { /* Convert from u-meters to milimeters. */
          wmm = di.width  / 1000.0;
          hmm = di.height / 1000.0;
          tried = True;
        }
# endif /* HAVE_OMAPFB */
# ifdef HAVE_FB
      if (!tried)
        {
          struct fb_var_screeninfo fi;

          /* Actually, I haven't seen this method working. */
          memset(&fi, 0, sizeof(fi));
          if (ioctl(hfb, FBIOGET_VSCREENINFO, &fi) == 0
              && (int)fi.width > 0 && (int)fi.height > 0)
            {
              wmm = fi.width;
              hmm = fi.height;
              tried = True;
            }
        }
# endif /* HAVE_FB */
      close(hfb);
    } /* Haven't $tried and /dev/fb is accessible. */

  if (tried > 0)
    {
      if (wmmp)
        *wmmp = wmm;
      if (hmmp)
        *hmmp = hmm;
      return True;
    }
  else
    tried = -True;
#endif /* HAVE_FB || HAVE_OMAPFB */

#ifdef HAVE_FREMANTLE
  /* Fortunately we know the px/mm density. */
  if (wmmp)
    *wmmp = DpyWidth / 10.5;
  if (hmmp)
    *hmmp = DpyHeight / 10.5;
  return True;
#else /* Give in to the lies. */
  if (wmmp)
    *wmmp = DisplayWidthMM(Dpy, Scr);
  if (hmmp)
    *hmmp = DisplayHeightMM(Dpy, Scr);
  return False;
#endif /* ! HAVE_FREMANTLE */
} /* get_dimensions */

/* If $p points at a known length suffix returns how much to scale
 * the measure to get milimeters and where to pick up parsing from.
 * Otherwise just returns $p. */
static char const *scale2mm(char const *p, float *scalep)
{
  static struct { char const *postfix; float scale; } const scales[] =
    { { "px", 0 }, { "mm", 1 }, { "cm", 10 }, { "in", 25.4 } };
  char const *pp;
  unsigned i;

  for (i = 0; i < MEMBS_OF(scales); i++)
    if ((pp = isprefix(p, scales[i].postfix)) != NULL)
      {
        *scalep = scales[i].scale;
        return pp;
      }

  *scalep = 0;
  return p;
} /* scale2mm */

/* Converts the length in $n to pixels using $scale to convert it
 * to milimeters first. */
static int mm2px(Bool is_width, int n, float scale)
{
  float mm;
  unsigned px;

  if (!scale)
    return n;

  if (is_width)
    {
      px = DpyWidth;
      get_dimensions(&mm, NULL);
    }
  else
    {
      px = DpyHeight;
      get_dimensions(NULL, &mm);
    }

  return n * scale * px/mm;
} /* mm2px */

/* scale2mm() and mm2px() in one go. */
static char const *scale2px(char const *p, short *np, Bool is_width)
{
  float scale;

  p = scale2mm(p, &scale);
  *np = mm2px(is_width, *np, scale);
  return p;
} /* scale2px */

/* Parse and return the number $p starts with.  Floating point numbers
 * are returned in $fp, integers in $np and $fp is set to zero.  Returns
 * the start of the next substring or NULL of $p couldn't be parsed. */
static char const *get_int_or_float(char const *p, int *np, float *fp)
{
  int n;
  unsigned d;

  *fp = 0;
  *np = d = 0;
  if (sscanf(p, ".%u%n", &d, &n) >= 1
      || sscanf(p, "%*[+-].%u%n", &d, &n) >= 1)
    *np = 0;
  else if (sscanf(p, "%d.%u%n", np, &d, &n) >= 2)
    *fp = *np;
  else if (sscanf(p, "%d%n", np, &n) >= 1)
    d = 0;
  else
    return NULL;

  if (d > 0)
    {
      for (*fp = d; d > 0; d /= 10)
        *fp /= 10;
      *fp += *np;
      if (!*np && *p == '-')
        /* "-.5" */
        *fp = -*fp;
    }

  return p+n;
} /* get_int_or_float */

static char const *get_short_or_float(char const *p, short *np, float *fp)
{
  int i;

  if ((p = get_int_or_float(p, &i, fp)) != NULL
      && *fp && (i < SHRT_MIN || SHRT_MAX < i))
    die("integer value out of range\n");
  *np = i;
  return p;
} /* get_short_or_float */

/* Convert $name to an Atom, resolving shortcuts. */
static Atom get_atom(char const *name)
{
  if (!strcmp(name, "support"))
    name = "_HILDON_PORTRAIT_MODE_SUPPORT";
  else if (!strcmp(name, "request"))
    name = "_HILDON_PORTRAIT_MODE_REQUEST";
  else if (!strcmp(name, "noncomp") || !strcmp(name, "nc"))
    name = "_HILDON_NON_COMPOSITED_WINDOW";
  else if (!strcmp(name, "dnd"))
    name = "_HILDON_DO_NOT_DISTURB";
  else if (!strcmp(name, "dnd_override"))
    name = "_HILDON_DO_NOT_DISTURB_OVERRIDE";
  else if (!strcmp(name, "parent"))
    name = "_HILDON_ANIMATION_CLIENT_MESSAGE_PARENT";
  else if (!strcmp(name, "show"))
    name = "_HILDON_ANIMATION_CLIENT_MESSAGE_SHOW";
  else if (!strcmp(name, "move"))
    name = "_HILDON_ANIMATION_CLIENT_MESSAGE_POSITION";
  else if (!strcmp(name, "anchor"))
    name = "_HILDON_ANIMATION_CLIENT_MESSAGE_ANCHOR";
  else if (!strcmp(name, "rotate"))
    name = "_HILDON_ANIMATION_CLIENT_MESSAGE_ROTATION";
  else if (!strcmp(name, "scale"))
    name = "_HILDON_ANIMATION_CLIENT_MESSAGE_SCALE";
  return XInternAtom(Dpy, name, False);
} /* get_atom */

/* Parses $in for KEY[=VALUE], converts KEY to an Atom and returns it
 * in $akey, then returns VALUE as a string or NULL if there isn't
 * any value. */
static char const *get_key_val(Atom *akey, char const *in)
{
  char *key;
  size_t lkey;

  /* Find the end of the $key. */
  for (lkey = 0; in[lkey] != '='; lkey++)
    if (in[lkey] == '\0')
      { /* There's no VALUE. */
        *akey = get_atom(in);
        return NULL;
      }

  key = mkstr(in, lkey);
  *akey = get_atom(key);
  free(key);

  return &in[lkey+1];
} /* get_key_val */

/*
 * Given an $str = "akarmi,valami,semmi" returns:
 *        *startp <-'.___/ `-> return value
 *                     ^
 *                   *lenp
 *
 * ie. the start and lenght of the substring (argument), and the start
 * of the next substring, or NULL if all arguments have been processed
 * or if there weren't any.
 */
static char const *get_optarg(char const *str,
                              char const **startp, size_t *lenp)
{
    int endc;

    if (!*str)
      return NULL;
    endc = *str == '"' || *str == '\'' ? *str++ : '\0';
    *startp = str;

    if (lenp)
      *lenp = 0;
    while (*str != endc)
      {
        switch (*str++)
          {
          case '\0':
            die("unterminated string\n");
          case ',':
            if (!endc)
              return str;
          }
        if (lenp)
          (*lenp)++;
      } /* for */

    if (!endc)
      /* We've reached the end of the string, no more arguments. */
      return str;

    /* Skip the delimiters. */
    str++;
    if (*str == ',')
      str++;
    return str;
} /* get_optarg */

/* Duplicate the current get_optarg() of $str and return it in *$argp. */
static char const *dup_optarg(char const *str, char **argp)
{
  size_t lstr;
  char const *next;

  if ((next = get_optarg(str, &str, &lstr)) != NULL)
    *argp = mkstr(str, lstr);
  return next;
} /* dup_optarg */

/* Parses a $str as "<thing>,..." until the end of the string,
 * adding <thing>s (either <int>s or <atom>s) to $list, at most
 * $max of them.  Returns the number of items parsed. */
static unsigned get_int_list(long *list, unsigned max, char const *str)
{
  unsigned i;

  memset(list, 0, sizeof(*list) * max);
  if (!str)
    return 0;

  i = 0;
  for (;;)
    {
      if (i >= max)
        die("too many arguments\n");
      else if (!*str)
        break;

      if (*str == ',')
        {
          list++;
          str++;
          i++;
        }
      else if (!(isdigit(*str) || *str == '-' || *str == '+' || isspace(*str)))
        {
          char *name;

          assert((str = dup_optarg(str, &name)) != NULL);
          if (strcmp(name, "none"))
            /* Not "none". */
            *list++ = get_atom(name);
          free(name);
        }
      else
        *list = strtol(str, (char **)&str, 0);
    } /* for */

  return i;
} /* get_int_list */

/*
 * Parse an expression specifying duration is seconds or miliseconds
 * and return it in miliseconds.
 *
 * duration := <float0+>                      # seconds
 * duration := <unsigned>                     # depends on $isms
 * duration := <unsigned>|<float0+> "s"|"ms"  # explicit
 */
static char const *get_duration(char const *p, unsigned *msp, Bool isms)
{
  int n;
  float f;

  /* Get the number. */
  if (!(p = get_int_or_float(p, &n, &f)))
    return NULL;
  if (f < 0 || (!f && n < 0))
    die("negative time\n");

  /* Does it denote seconds or milisecs? */
  if (p[0] == 'm' && p[1] == 's')
    {
      p += 2;
      isms = 1;
    }
  else if (p[0] == 's')
    {
      p++;
      isms = 0;
    }
  else if (f)
    /* Fractional milisecs don't make sense. */
    isms = 0;

  if (isms)
    *msp = f ? f : n;
  else
    *msp = f ? f*1000 : n*1000;

  return p;
} /* get_duration */

/*
 * Get a pair of "<int>|<rel>[<off>]" and interpret them either as <dim>s
 * or <coord>s, depending on $isdim.  if $xpos, an 'x' is expected between
 * the elements of the pair.  $y_rel_off indicates whether the second element
 * may take a <rel><off> form, or the offset should be left unparsed.
 */
static char const *get_dims_or_coords(char const *p,
                                      short *xp, short *yp,
                                      Bool isdim, Bool y_rel_off,
                                      Bool xpos)
{
  float rel, scale;

  /* First coordinate/dimension. */
  if (!(p = get_short_or_float(p, xp, &rel)))
    /* Unparseable. */
    return NULL;
  if (rel)
    {
      *xp = DpyWidth * rel;
      if (!xpos || *p != 'x')
        {
          /*
           * In $xpos 'x' is a reliable delimiter; otherwise we either have
           * 1) "<rel><rel>"      ("0.5_+0.5_"),          // !$xpos
           * 2) "<rel><rel><off>" ("0.5_+0.4_+10")        // !$xpos
           * 3) "<rel><off>x"     ("0.5_+10x_20")         //  $xpos
           * 4) "<rel><off><rel>" ("0.5_+10+_0.4+10")     // !$xpos
           * 5) "<rel><off>"      ("0.5_+10_").           // !$xpos
           * where the first <rel> is already parsed.
           */
          if (!(p = get_short_or_float(p, yp, &rel)))
            return 0;

          /* In $xpos a <rel> must be followed by an <off>. */
          if (xpos && rel)
            return NULL;

          if (rel)
            /* Cases 1 and 2 */
            goto have_y_rel;
          p = scale2mm(p, &scale);
          if (xpos || *p == '+' || *p == '-')
            /* Cases 3 and 4, <off> is in *$yp. */
            *xp += mm2px(True, *yp, scale);
          else
            { /* Case 5, we're done because <off> is in *$yp. */
              *yp = mm2px(False, *yp, scale);
              return p;
            }
        }
    } /* if first component is relative */
  else
    {
      p = scale2px(p, xp, True);
      if (isdim && *xp < 0)
        /* Count from the other edge. */
        *xp += DpyWidth;
    }

  /* Second component. */
  if (xpos && *p++ != 'x')
    return NULL;
  if (!(p = get_short_or_float(p, yp, &rel)))
    /* Unparseable. */
    return NULL;
  if (rel)
    {
have_y_rel:
      *yp = DpyHeight * rel;
      if (y_rel_off)
        { /* Is it followed by an offset? */
          short off;
          char const *oldp;

          oldp = p;
          off = strtol(p, (char **)&p, 10);
          if (oldp < p)
            {
              p = scale2px(p, &off, False);
              *yp += off;
            }
        }
    }
  else
    {
      p = scale2px(p, yp, False);
      if (isdim && *yp < 0)
        /* Count from the other edge. */
        *yp += DpyHeight;
    }

  /* Don't return negative dimensions. */
  if (isdim)
    {
      if (*xp < 0)
        *xp = 0;
      if (*yp < 0)
        *yp = 0;
    }

  return p;
} /* get_dims_or_coords */

/* Get a <pos> or an <xpos>. */
static char const *get_point(char const *p, short *xp, short *yp,
                             char const **originp, Bool xpos)
{
  char const *pp, *origin;

  if (!(pp = get_dims_or_coords(p, xp, yp, False, True, xpos)))
    { /* No coordinates, maybe we have a [tcb][lcr]. */
      origin = p;
      *xp = *yp = 0;
    }
  else
    origin = pp;

  if (!((origin[0] == 't' || origin[0] == 'c' || origin[0] == 'b')
        && (origin[1] == 'l' || origin[1] == 'c' || origin[1] == 'r')))
    {
      /* No origin, $p was all to parse. */
      if (originp)
        *originp = NULL;
      return pp;
    }

  /* Translate; the origin is tl by default. */
  if (originp)
    *originp = origin;
  if (origin[1] == 'c')
    *xp += DpyWidth / 2;
  else if (origin[1] == 'r')
    *xp = DpyWidth  - *xp;
  if (origin[0] == 'c')
    *yp = DpyHeight / 2;
  else if (origin[0] == 'b')
    *yp = DpyHeight - *yp;

  return origin+2;
} /* get_point */

/* Simplified get_point(). */
static char const *get_xpos(char const *p, XPoint *xpos)
{
  return get_point(p, &xpos->x, &xpos->y, NULL, True);
} /* get_xpos */

/* Get a <geo>.  See the user documentation for examples. */
static char const *get_geometry(char const *str, XRectangle *geo)
{
  char const *p1, *pp, *origin;

  if (str[0] == 'f' && str[1] == 's')
    { /* Fullscreen */
      geo->x = geo->y = 0;
      geo->width  = DpyWidth;
      geo->height = DpyHeight;
      return str + 2;
    }

  if (!(p1 = get_dims_or_coords(str,
                                (short*)&geo->width, (short*)&geo->height,
                                True, True, True)))
    die("invalid geometry\n");
  if (!(pp = get_point(p1, &geo->x, &geo->y, &origin, False)))
    { /* Recognize {<dim>x<rel>}{<off><coord>} (eg. 100x0.5+10+0.5). */
      short w2, h2;
      if ((pp = get_dims_or_coords(str, &w2, &h2, True, False, True)) != NULL
          && (pp = get_point(pp, &geo->x, &geo->y, &origin, False)) != NULL)
        { /* Yes, the alternative parsing was successful. */
          geo->width  = w2;
          geo->height = h2;
        }
      else
        { /* No, it looks <pos> was really not present. */
          geo->x = geo->y = 0;
          pp = p1;
        }
    }

  if (origin)
    { /* The user actually gave the $origin coordinate of the box. */
      if (origin[1] == 'c')
        geo->x -= geo->width / 2;
      else if (origin[1] == 'r')
        geo->x -= geo->width;
      if (origin[0] == 'c')
        geo->y -= geo->height / 2;
      else if (origin[0] == 'b')
        geo->y -= geo->height;
    }

  assert(pp != NULL);
  return pp;
} /* get_geometry */

/* Resolve $str as a color name. */
static char const *get_color_by_name(Colormap cmap, char const *str,
                                     XColor *xcolor)
{
  char *dup;
  unsigned len;
  char const *color;

  /* Parse it as a color name until a non-word character. */
  for (len = 0; isalnum(str[len]); len++)
      ;
  if (!str[len])
    {
      color = str;
      dup   = NULL;
    }
  else
    color = dup = mkstr(str, len);

  if (cmap == None)
    cmap = DefaultColormap(Dpy, Scr);
  if (!XParseColor(Dpy, cmap, color, xcolor))
    {
      if (!strcmp(color, "random") || !strcmp(color, "rand")
          || !strcmp(color, "rnd"))
        {
          memset(xcolor, 0, sizeof(*xcolor));
          xcolor->red   = rand();
          xcolor->green = rand();
          xcolor->blue  = rand();
        }
      else
        die("unknown color\n");
    }

  assert(XAllocColor(Dpy, cmap, xcolor));
  free(dup);

  return &str[len];
} /* get_color_by_name */

/* Returns the XColor of the <color> in $cmap specified by $str,
 * including the possible <alpha> in the color pixel value. */
static char const *get_xcolor(Colormap cmap, char const *str, XColor *xcolor)
{
  char *endp;
  Bool has_alpha;
  unsigned long alpha;

  /* Verify that $str is a <color> specification.
   * $str can be: #<alpha>, %<alpha>, #<alpha>@<color-name>,
   * @<color-name>, or @<color-name>%<alpha>. */
  assert(str[0] == '@' || str[0] == '%' || str[0] == '#');

  /* Leading <alpha>? */
  alpha = 0xff;
  has_alpha = False;
  if (*str == '#' || *str == '%')
    {
      str++;
      has_alpha = True;
      alpha = strtoul(str, (char **)&endp, 0);
      if (!(str < endp))
        die("missing alpha from color specification\n");
      str = endp;
    }

  /* Get the $xcolor. */
  if (*str == '@')
    {
      /* Did we get a pixel value? */
      str++;
      xcolor->pixel = strtoul(str, &endp, 0);
      if (str < endp)
        { /* Yes we did, fill in the rest of $xcolor. */
          XQueryColor(Dpy, cmap, xcolor);
          str = endp;
        }
      else
        /* It's a <color-name>. */
        str = get_color_by_name(cmap, str, xcolor);
    }
  else
    {
      assert(has_alpha);
      memset(xcolor, 0, sizeof(*xcolor));
    }

  /* Closing <alpha>? */
  if (*str == '%')
    {
      if (has_alpha)
        /* We already had a '#' or '%'. */
        die("double alpha in color specification\n");

      str++;
      has_alpha = True;
      alpha = strtoul(str, (char **)&endp, 0);
      if (!(str < endp))
        die("missing alpha from color specification\n");
      str = endp;
    }

  xcolor->pixel &= 0x00ffffff;
  xcolor->pixel |= alpha << 24;

  return str;
} /* get_xcolor */

/* Get just the pixel value in $cmap of a <color>. */
static char const *get_color_pixel(Colormap cmap, char const *str,
                                   unsigned long *pixel)
{
  XColor xcolor;

  str = get_xcolor(cmap, str, &xcolor);
  *pixel = xcolor.pixel;
  return str;
} /* get_color_pixel */

/* Return whether $name matches $rname as window name. */
static Bool cmp_window_names(char const *name, char const *rname)
{
  return name[0] == '!'
             ?  !strcmp(&name[1], rname)
             : !!strcasestr(rname, name);
} /* cmp_window_names */

/* Return $win's _NET_WM_NAME or NULL. */
static char *get_net_wm_name(Window win)
{
  int foo;
  unsigned long bar;
  Atom net_wm_name, rtype;
  unsigned char *name;

  net_wm_name = XInternAtom(Dpy, "_NET_WM_NAME", False);
  return XGetWindowProperty(Dpy, win, net_wm_name, 0, 64, False,
                         Utf8, &rtype, &foo, &bar, &bar,
                         &name) == Success
         && rtype == Utf8
    ? (char *)name : NULL;
} /* get_net_wm_name */

/*
 * Finds the client window of *$win, which is supposed to be the
 * window manager's frame window and returns it in the same location.
 * Returns whether it managed to find the client window.  Based on
 * XmuClientWindow() code.
 */
static Bool find_client_window(Window *win, char const *name,
                               char const *wintype)
{
  Bool ours;
  unsigned nchildren;
  Window root, parent, *children;

  /* Is $win what we are looking for? */
  ours = False;
  trap_xerrors();
  if (name)
    {
      char *rname;
      XClassHint cls;

      if (XFetchName(Dpy, *win, &rname) && rname)
        {
          ours = cmp_window_names(name, rname);
          XFree(rname);
        }
      if (!ours && (rname = get_net_wm_name(*win)) != NULL)
        {
          ours = cmp_window_names(name, rname);
          XFree(rname);
        }
      if (!ours && XGetClassHint(Dpy, *win, &cls))
        {
          ours = cmp_window_names(name, cls.res_name)
            || cmp_window_names(name, cls.res_class);
          XFree(cls.res_name);
          XFree(cls.res_class);
        }
    }

  if (!name || ours)
    { /* Is *$win a client window? */
      XWindowAttributes attrs;

      /* Reject stupid 1x1 windows. */
      ours = XGetWindowAttributes(Dpy, *win, &attrs)
        && (attrs.width > 1 && attrs.height > 1);
      if (ours && wintype)
        { /* Match $wintype. */
          static Atom wm_type = None;
          Atom proptype;
          int foo;
          unsigned long bar, baz;
          unsigned char *data;

          if (wm_type == None)
            wm_type = XInternAtom(Dpy, "_NET_WM_WINDOW_TYPE", False);
          if (XGetWindowProperty(Dpy, *win, wm_type, 0, 1, False,
                                 XA_ATOM, &proptype, &foo, &bar,
                                 &baz, &data) == Success
              && proptype == XA_ATOM)
            {
              char *hastype;

              hastype = XGetAtomName(Dpy, *(Atom *)data);
              ours = !!strstr(hastype, wintype);
              XFree(hastype);
              XFree(data);
            }
          else
            ours = False;
        }

      if (ours)
        return True;
    }

  /* See its children from the top to the bottom.  The order of traversal
   * makes it possible to find the topmost window. */
  assert(!ours);
  children = NULL;
  if (XQueryTree(Dpy, *win, &root, &parent, &children, &nchildren))
    {
      while (nchildren > 0
             && !((ours = find_client_window(&children[--nchildren],
                                             name, wintype))))
        ;
      /* Don't trash $win if we didn't find the window to allow the caller
       * to try alternatives. */
      if (ours)
        *win = children[nchildren];
    }
  if (children)
    XFree(children);

  /* We can ignore async errors because all the commands we use
   * have a return value. */
  untrap_xerrors();
  return ours;
} /* find_client_window */

/* Wait until the user clicks a window and return its XID.  Unless the user
 * right-clicked return clicked (frame) window's client window.  On middle
 * button click return None to indicate cancel.  Based on xwininfo code. */
static char const *pick_window(Window *winp)
{
  int x, y;
  Bool released;
  Cursor cursor;
  char const *what;
  enum { NONE, CANCEL, FRAME, CLIENT, WIDGET } pressed;

  /* Grab the pointer then wait for a full ButtonPress-Release cycle. */
  cursor = XCreateFontCursor(Dpy, XC_crosshair);
  assert(XGrabPointer(Dpy, Root, False,
                      ButtonPressMask|ButtonReleaseMask,
                      GrabModeSync, GrabModeAsync,
                      Root, cursor, CurrentTime) == GrabSuccess);

  pressed = NONE;
  released = False;
  while (!released)
    {
      XEvent event;

      XAllowEvents(Dpy, SyncPointer, CurrentTime);
      XWindowEvent(Dpy, Root, ButtonPressMask|ButtonReleaseMask, &event);
      switch (event.type)
        {
          case ButtonPress:
            if ((*winp = event.xbutton.subwindow) == None)
              *winp = Root;
            x = event.xbutton.x;
            y = event.xbutton.y;
            if (Verbose && Is_interactive)
              printf("%dx%d\n", x, y);
            if (event.xbutton.button == 2)
              pressed = CANCEL;
            else if (event.xbutton.button == 3)
              pressed = FRAME;
            else if (event.xbutton.state & 1)
              pressed = WIDGET;
            else
              pressed = CLIENT;
            break;
          case ButtonRelease:
            if (pressed != NONE)
              released = True;
            break;
        }
    }  /* while */

  if (pressed == CANCEL)
    {
      what = NULL;
      goto out;
    }
  else if (pressed == WIDGET)
    { /* Find the topmost X window. */
      Window parent, child;

      what = "widget";
      parent = Root;
      for (;;)
        {
          assert(XTranslateCoordinates(Dpy, parent, *winp,
                                       x, y, &x, &y, &child));
          if (child == None)
            break;
          parent = *winp;
          *winp = child;
        }
    }
  else if (pressed == CLIENT && *winp != Root)
    {
      what = "client window";
      find_client_window(winp, NULL, NULL);
    }
  else
    what = "frame window";

out:
  XUngrabPointer(Dpy, CurrentTime);
  XFreeCursor(Dpy, cursor);
  return what;
} /* pick_window */

/* Returns the current active window's XID. */
static char *get_property(Atom win, Atom key, Atom type);
static Window find_topmost(void)
{
  static char const *props[] =
    {
      "_MEEGOTOUCH_CURRENT_APP_WINDOW",
      "_MB_CURRENT_APP_WINDOW",
      "_NET_ACTIVE_WINDOW",
      NULL
    };
  Window top;
  unsigned i;
  char *val;

  for (i = 0; props[i]; i++)
    if ((val = get_property(Root, XInternAtom(Dpy, props[i], False),
                            XA_WINDOW)) != NULL)
      {
        top = *(Window *)val;
        XFree(val);
        return top;
      }

  top = Root;
  if (!find_client_window(&top, NULL, NULL))
    die("no topmost window\n");
  return top;
} /* find_topmost */

/* Find the window manager's information board. */
static Window find_wm_window(void)
{
  Atom wm_check;
  Window win, *wm1, *wm2;

  win = None;
  wm1 = wm2 = NULL;
  trap_xerrors();
  wm_check = XInternAtom(Dpy, "_NET_SUPPORTING_WM_CHECK", False);
  if ((wm1 = (Window *)get_property(Root, wm_check, XA_WINDOW)) != NULL)
    { /* Verify that the window exists. */
      if ((wm2 = (Window *)get_property(*wm1, wm_check,
                                        XA_WINDOW)) != NULL)
        { /* Verify that it really is the wm's window. */
          if (*wm1 == *wm2)
            win = *wm1;
          XFree(wm2);
        }
      XFree(wm1);
    }
  untrap_xerrors();

  return win;
} /* find_wm_window */

/* Finds out which window $str refers to. */
static Window choose_window(char const *str)
{
  char *endp;
  Window win;
  char const *what;

  what = str;
  if (!strcmp(str, "select"))
    { /* Pointer-pick the window. */
      if (Verbose && Is_interactive)
        {
          fputs("You clicked at... ", stdout);
          fflush(stdout);
        }
      if (!(what = pick_window(&win)))
        {
          puts("nothing");
          return None;
        }
    }
  else if (!strcmp(str, "new"))
    win = Newborn;
  else if (!strcmp(str, "root"))
    win = Root;
#if HAVE_XCOMPOSITE
  else if (!strcmp(str, "overlay"))
    win = XCompositeGetOverlayWindow(Dpy, Root);
#endif
  else if (!strcmp(str, "wm"))
    {
      if (!(win = find_wm_window()))
        die("no window manager running\n");
    }
  else if (!strcmp(str, "top"))
    win = find_topmost();
  else if (isprefix(str, "top-"))
    {
      win = Root;
      if (!find_client_window(&win, NULL, &str[4]))
        die("no such window\n");
    }
  else
    { /* Either an XID or a window name. */
      win = strtoul(str, &endp, 0);
      if (!*str || *endp)
        {
          win = Root;
          if (!find_client_window(&win, str, NULL))
            die("no such window\n");
        }
      else if (win == 0)
        win = Root;
      else
        what = "literal";
    } /* if */

  if (Verbose)
    {
      if (Is_interactive)
        printf("Window 0x%lx (%s)\n", win, what);
      else
        printf("0x%lx\n", win);
    }

  return win;
} /* choose_window */

/* Force a user to choose window, ie. it cannot be cancelled. */
static Window __attribute__((unused))
must_choose_window(char const *str)
{
  Window win;

  if (!(win = choose_window(str)))
      die("you have to choose a window\n");
  return win;
} /* must_choose_window */

/* Returns the significant bits of $px. */
static unsigned char mask_pixel(unsigned px, unsigned mask)
{
  assert(mask);
  while (mask > 0xff)
    {
      px   >>= 1;
      mask >>= 1;
    }
  while (mask < 0x80)
    {
      px   <<= 1;
      mask <<= 1;
    }
  return px & mask;
} /* mask_pixel */

/*
 * Calculates the intersection of [$x, $x+$w] and [0, $maxw],
 * two non-zero intervals, and returns the width of the resulting
 * interval or 0 if the intervals don't overlap.  If $xp is not NULL
 * and there is an intersection, *$xp is set to the start of it.
 */
static unsigned intersect(int *xp, int x, unsigned w, unsigned maxw)
{
  assert(w > 0 && maxw > 0);

  if (x < 0)
    {
      if ((unsigned)-x > w-1)
        return 0;
      w += x;
      x  = 0;
    }
  else
    {
      if (maxw <= (unsigned)x)
        return 0;
      maxw -= x;
    }

  if (w > maxw)
    w = maxw;
  assert(0 <= x);
  assert(w <= maxw);

  if (xp)
    *xp = x;
  return w;
} /* intersect */

/* Return whether $win is a Window or a Pixmap, and try to fill in $attrs
 * as completely as possible and necessary. */
static Bool get_win_attrs(Drawable win, XWindowAttributes *attrs,
                          Bool handle_pixmaps, Visual *visual)
{
  unsigned error;

  trap_xerrors();
  XGetWindowAttributes(Dpy, win, attrs);
  if (!(error = untrap_xerrors()))
    return True;
  if (error == BadWindow)
    die("window does not exist\n");

  /* $win is a pixmap. */
  assert(error == BadMatch);
  memset(attrs, 0, sizeof(*attrs));
  attrs->colormap = DefaultColormap(Dpy, Scr);
  if (handle_pixmaps)
    {
      XVisualInfo vi;

      assert(XGetGeometry(Dpy,
             win, &attrs->root,
             &attrs->x, &attrs->y,
             (unsigned *)&attrs->width, (unsigned *)&attrs->height,
             (unsigned *)&attrs->border_width, (unsigned *)&attrs->depth));
      if (visual)
        {
          assert(XMatchVisualInfo(Dpy, Scr, attrs->depth, TrueColor, &vi));
          visual->red_mask   = vi.red_mask;
          visual->green_mask = vi.green_mask;
          visual->blue_mask  = vi.blue_mask;
          attrs->visual = visual;
        }
    }
  return False;
} /* get_win_attrs */

/* Returns the PID of the owner of $win.
 * (The process may not exist anymore.) */
static pid_t get_client_pid(Drawable win, Bool ispixmap)
{
  Atom pida;
  pid_t *pidp, pid;

#ifdef HAVE_XRES_12
  long nclients;
  XResClientIdSpec spec;
  XResClientIdValue *clients;

  /* First try Xres, which is more reliable. */
  pid         = -1;
  nclients    = 0;
  spec.client = win;
  spec.mask   = XRES_CLIENT_ID_PID_MASK;
  XResQueryClientIds(Dpy, 1, &spec, &nclients, &clients);
  if (nclients > 0)
    pid = XResGetClientPid(&clients[0]);
  XResClientIdsDestroy(nclients, clients);
  if (nclients > 0)
    return pid;
#endif /* HAVE_XRES_12 */

  /* Read the _NET_WM_PID property if $win is a window. */
  if (ispixmap)
    return -1;

  pida = XInternAtom(Dpy, "_NET_WM_PID", False);
  pidp = (pid_t *)get_property(win, pida, XA_CARDINAL);
  if (pidp)
    {
      pid = *pidp;
      XFree(pidp);
    }
  else
    pid = -1;

  return pid;
} /* get_client_pid */

/* Decodes an X event $mask and returns the textual representation. */
static char const *decode_event_mask(long mask)
{
  static char const *const tbl[] =
  {
    "KeyPress", "KeyRelease", "ButtonPress", "ButtonRelease",
    "EnterWindow", "LeaveWindow", "PointerMotion", "PointerMotionHint",
    "Button1Motion", "Button2Motion", "Button3Motion", "Button4Motion",
    "Button5Motion", "ButtonMotion", "KeymapState", "Exposure",
    "VisibilityChange", "StructureNotify", "ResizeRedirect",
    "SubstructureNotify", "SubstructureRedirect", "FocusChange",
    "PropertyChange", "ColormapChange", "OwnerGrabButton",
  };

  unsigned i;
  char const *buf;

  if (!mask)
    return "none";

  buf = addstr(NULL);
  for (i = 0; i < MEMBS_OF(tbl); i++)
    if (mask & (1 << i))
      {
        if (buf[0])
          addstr(", ");
        buf = addstr(tbl[i]);
      }

  return buf;
} /* decode_event_mask */

/* Prints $attrs->*mask*. */
static void print_event_masks(XWindowAttributes const *attrs,
                              unsigned indent)
{
  printf("%*s  all event mask: %s\n", indent, "",
         decode_event_mask(attrs->all_event_masks));
  printf("%*s your event mask: %s\n", indent, "",
         decode_event_mask(attrs->your_event_mask));
  printf("%*snopropagate mask: %s\n", indent, "",
         decode_event_mask(attrs->do_not_propagate_mask));
} /* print_event_masks */

/* Retrieves and prints $win's shape rectangles of $type,
 * ShapeInput or ShapeBounding. */
#ifdef HAVE_XEXT
static void print_shape(Window win, int type)
{
  int i;
  XRectangle *shape;
  int nrects, unused;

  shape = XShapeGetRectangles(Dpy, win, type, &nrects, &unused);
  for (i = 0; i < nrects; i++)
    printf(" %dx%d%+d%+d",
           shape[i].width, shape[i].height,
           shape[i].x, shape[i].y);
  putchar('\n');
  XFree(shape);
} /* print_shape */
#endif /* HAVE_XEXT */

/* Print if $win is composited. */
static void print_redirection(Window win, unsigned indent)
{
#ifdef HAVE_XCOMPOSITE
  Pixmap pixmap;

  trap_xerrors();
  pixmap = XCompositeNameWindowPixmap(Dpy, win);
  if (untrap_xerrors() == Success)
    {
      printf("%*sRedirected to:  0x%lx\n", indent, "", pixmap);
      XFreePixmap(Dpy, pixmap);
    }
#endif /* HAVE_XCOMPOSITE */
} /* print_redirection */

/* Print some of $win's attributes and its shape. */
static void print_info(Drawable win, Bool recursive, unsigned level)
{
  pid_t pid;
  char *name;
  XClassHint cls;
  Bool is_window;
  XWindowAttributes attrs;
  unsigned indent, nchildren, i;
  Window root, parent, *children;

  /* If $Verbose or we're recusrive the XID is already printed. */
  indent = (level+1) * 2;
  if (!Verbose && !level)
    printf("0x%lx\n", win);

  /* Is $win a Window or a Pixmap? */
  is_window = get_win_attrs(win, &attrs, True, NULL);

  /* Print the window's name. */
  name = cls.res_name = cls.res_class = NULL;
  if (is_window)
    { /* Pixmap don't have a name. */
      XFetchName(Dpy, win, &name);
      XGetClassHint(Dpy, win, &cls);
    }
  pid = get_client_pid(win, !is_window);
  if (name || cls.res_name || cls.res_class || !is_window)
    {
      printf("%*s", indent, "");
      if (name && cls.res_name && cls.res_class)
        { /* We have all the names. */
          printf("%s (%s, %s", name, cls.res_name, cls.res_class);
          if (pid > 0)
            printf(", %d", pid);
          putchar(')');
        }
      else if (!!name + !!cls.res_name + !!cls.res_class == 2)
        { /* We have two. */
          printf("%s (%s",
                 name         ?         name : cls.res_name,
                 cls.res_name ? cls.res_name : cls.res_class);
          if (pid > 0)
            printf(", %d", pid);
          putchar(')');
        }
      else
        { /* We have one or zero. */
          fputs(name            ? name
                : cls.res_name  ? cls.res_name
                : cls.res_class ? cls.res_class
                :                 "[pixmap]",
                stdout);
          if (pid > 0)
            printf(" (%d)", pid);
        }
      putchar('\n');

      if (name)
        XFree(name);
      if (cls.res_name)
        XFree(cls.res_name);
      if (cls.res_class)
        XFree(cls.res_class);
    }
  else if (win == Root)
    {
      printf("%*s%s", indent, "", "Root window");
      if (pid > 0)
        printf(" (%d)", pid);
      putchar('\n');
    }
  else if (pid > 0)
    printf("%*s[pid=%d]\n", indent, "", pid);

  /* Window state.  $Root is always mapped. */
  if (is_window && (win != Root || Verbose))
    {
      printf("%*sState:          %s", indent, "",
               attrs.map_state == IsUnmapped   ? "unmapped"
             : attrs.map_state == IsUnviewable ? "mapped (unviewable)"
             : attrs.map_state == IsViewable   ? "mapped"
             :                                   "wtf");
      if (attrs.override_redirect)
        fputs(" (override-redirected)", stdout);
      putchar('\n');
    }

  /* Depth ("the number of significant bits per pixel"). */
  printf("%*sDepth:          %d bit", indent, "", attrs.depth);
  if (attrs.c_class == InputOnly)
    fputs(" (InputOnly)", stdout);
  putchar('\n');

  /* Geometry.  Pixmaps only have size. */
  printf("%*sGeometry:       %dx%d", indent, "",
         attrs.width, attrs.height);
  if (is_window)
    printf("%+d%+d", attrs.x, attrs.y);
  putchar('\n');

#ifdef HAVE_XEXT
  if (is_window)
    { /* Window shapes */
      printf("%*sBounding shape:", indent, "");
      print_shape(win, ShapeBounding);
      printf("%*sClipping shape:", indent, "");
      print_shape(win, ShapeClip);
      printf("%*sInput shape:   ", indent, "");
      print_shape(win, ShapeInput);
    }
#endif

  if (win == Root)
    { /* Show general display information. */
      int revert;
      float wmm, hmm;

      /* Pixel and milimeter dimensions. */
      if (get_dimensions(&wmm, &hmm))
        printf("%*sDimensions:     %gx%gmm (%g\", %gx%g dpi)\n",
               indent, "", wmm, hmm,
               sqrt(wmm*wmm + hmm*hmm) / 25.4,
               25.4*DpyWidth / wmm, 25.4*DpyHeight / hmm);
        printf("%*sAspect ratio:   %g\n", indent, "",
               (100*DpyWidth/DpyHeight) / 100.0);

      /* Window focus. */
      XGetInputFocus(Dpy, &parent, &revert);
      printf("%*sFocused window: 0x%lx (%s)\n", indent, "", parent,
               revert == RevertToParent       ? "reverts to the parent window"
             : revert == RevertToPointerRoot  ? "reverts to the root window"
             : revert == RevertToNone         ? "doesn't revert to anywhere"
             :                                  "reverts to somewhere");
    }

  /* If $win is a pixmap, we've printed all information we could. */
  if (!is_window)
    return;
  print_redirection(win, indent);
  if (Verbose)
    print_event_masks(&attrs, indent);
  if (!recursive)
    return;

  /* Print children as well. */
  if (!XQueryTree(Dpy, win, &root, &parent, &children, &nchildren))
    return;
  for (i = 0; i < nchildren; i++)
    {
      win = children[nchildren-1-i];
      printf("%*sSubwindow 0x%lx:\n", indent, "", win);
      print_info(win, True, level+1);
    }
  XFree(children);
} /* print_info */

#ifdef HAVE_XRES
# ifndef HAVE_XRES_12
typedef int XResNClients;
typedef XResClient XResClientIdValue;

typedef struct
{
  XID client;
  unsigned mask;
} XResClientIdSpec;

/* These calls are only available in XRes 1.2.  To keep things simple
 * and make it possible to use the new interface unconditionally,
 * emulate what we can with the old interface. */
#  define XRES_CLIENT_ID_PID_MASK           0
#  define XResGetClientPid(v)             (-1)
#  define XResQueryClientIds(dpy, nspecs, specs, nclientsp, clients) \
          XResQueryClients(dpy, nclientsp, clients)
#  define XResClientIdsDestroy(nclients, clients)                   \
          XFree(clients)
#  define XRESCLIENT(client, i)           (clients)[i].resource_base
# else /* HAVE_XRES_12 */
typedef long XResNClients;
#  define XRESCLIENT(client, i)           (clients)[i].spec.client
# endif

/* qsort() comparator to order a list of XResType:s by resource_type. */
static int cmpxres(void const *lhs, void const *rhs)
{
  XResType const *lres = (XResType const *)lhs;
  XResType const *rres = (XResType const *)rhs;

  if (lres->resource_type < rres->resource_type)
    return -1;
  if (lres->resource_type > rres->resource_type)
    return  1;
  return 0;
} /* cmpxres */

/* Pretty-print $resources. */
static void print_resource_list(enum resource_listing_t what,
                                unsigned nresources, XResType *resources,
                                unsigned nclients, unsigned long spixmaps)
{
  static char const *nc = "number of clients";
  static char const *ps = "pixmap bytes";
  unsigned i;
  char **names;
  XResType *checkpoint;
  size_t maxlen, maxcount;

  if (what == SYSTEMWIDE)
    /* We'll need to sum up the counts of the same resource_type:s. */
    qsort(resources, nresources, sizeof(*resources), cmpxres);

  /* Allocate space for the names of the resource_type:s. */
  xmalloc(&names, sizeof(*names) * nresources);
  memset(names, 0, sizeof(*names)*nresources);

  /*
   * Walk through $resources and filter out those we shouldn't display
   * (because they're below the specified verbosity level), then sum up
   * the counts of the same resource_type:s if we're showing SYSTEMWIDE
   * statistics, and finally look up the name of the resource.
   */
  checkpoint = NULL;
  maxlen = maxcount = 0;
  for (i = 0; i < nresources; i++)
    {
      static char const *const whitelist0[] =
        {
          "COLORMAP", "PIXMAP", "WINDOW", "GC", "PICTURE",
          "ShmSeg", "DRI2Drawable", "pixmap bytes",
          NULL
        };
      static char const *const whitelist1[] =
        {
          "OTHER CLIENT", "INPUTCLIENT",
          "ShapeEvent", "ShapeClient", "DamageExt",
          "CompositeClientWindow", "CompositeClientSubwindows",
          NULL
        };
      static char const *const *const whitelists[] =
        { whitelist0, whitelist1 };

      if (what == EACH_CLIENT)
        {
          /* Skip the meta-element which describes how many elements
           * belong to this client. */
          if (!checkpoint
              || &resources[i]-checkpoint >= (int)checkpoint->count)
            { /* *$checkpoint := the client's meta-element */
              checkpoint = &resources[i++];
              continue;
            }
        }
      else if (what == SYSTEMWIDE)
        {
          if (checkpoint
              && checkpoint->resource_type == resources[i].resource_type)
            { /* Aggregate; the corresponding $names element will be NULL. */
              checkpoint->count += resources[i].count;
              resources[i].resource_type = None;
              continue;
            }
          else
            /* New resource type, the counts of same resource types
             * will be added to this one. */
            checkpoint = &resources[i];
        }

      /* Can we display this resource type? */
      names[i] = XGetAtomName(Dpy, resources[i].resource_type);
      if (Verbose < MEMBS_OF(whitelists))
        {
          char const *const *word;
          char const *const *const *whitelist;

          /* Check the whitelists. */
          for (whitelist = &whitelists[Verbose]; ; whitelist--)
            {
              for (word = *whitelist; *word; word++)
                if (!strcmp(names[i], *word))
                  goto whitelisted;
              if (whitelist <= whitelists)
                break;
            }

          /* Not found. */
          free(names[i]);
          names[i] = NULL;
          continue;
        }

whitelisted:
      /* Record the widths for pretty-printing. */
      maxlen   = larger(maxlen, strlen(names[i]));
      maxcount = larger(maxcount, resources[i].count);
    } /* for */

  /* Only if we show information about EACH_CLIENT $ps is included
   * in $resources, otherwise we print it separately.  When showing
   * SYSTEMWIDE statistics include $nc as well. */
  if (what != EACH_CLIENT)
    {
      if (what == SYSTEMWIDE)
        {
          maxlen   = larger(maxlen, strlen(nc));
          maxcount = larger(maxcount, nclients);
        }
      maxlen   = larger(maxlen, strlen(ps));
      maxcount = larger(maxcount, spixmaps);
    }

  /* $maxcount := the decimal width of $maxcount */
  maxlen++;
  for (i = 1; maxcount > 0; i++)
    maxcount /= 10;
  maxcount = i;

  /* Print the information not included in $resources. */
  if (what != EACH_CLIENT)
    {
      if (what == SYSTEMWIDE)
        printf("  %s:%*c%*u\n", nc, maxlen-strlen(nc), ' ',
               maxcount, nclients);
      printf("  %s:%*c%*lu\n", ps, maxlen-strlen(ps), ' ',
             maxcount, spixmaps);
    }

  /* Print $resources. */
  checkpoint = NULL;
  for (i = 0; i < nresources; i++)
    {
      if (what == EACH_CLIENT
          && (!checkpoint
              || &resources[i]-checkpoint >= (int)checkpoint->count))
        { /* Start of new client, print its XID and PID if possible. */
          checkpoint = &resources[i++];
          printf("Client 0x%lx:", checkpoint->resource_type);
          if (checkpoint[1].count != (unsigned)-1)
            {
              pid_t pid;
              ssize_t lexe;
              int cmdlinefd;
              char exe[PATH_MAX];

              /* Try to determine the executable from the $pid. */
              lexe = 0;
              pid  = checkpoint[1].count;

              /* Get argv[0]. */
              sprintf(exe, "/proc/%u/cmdline", pid);
              if ((cmdlinefd = open(exe, O_RDONLY)) >= 0)
                {
                  lexe = read(cmdlinefd, exe, sizeof(exe)-1);
                  close(cmdlinefd);
                }

              /* If it failed read the exe link.  This is less reliable,
               * because we might not have permission to do it and its
               * result is meaningless if the app is boosted. */
              if (lexe <= 0)
                {
                  sprintf(exe, "/proc/%u/exe", pid);
                  lexe = readlink(exe, exe, sizeof(exe)-1);
                }

              /* Print whatever we can. */
              if (lexe > 0)
                {
                  char const *base;

                  exe[lexe] = '\0';
                  if ((base = strrchr(exe, '/')) != NULL)
                    base++;
                  else
                    base = exe;
                  printf(" (%s, pid=%u)", base, pid);
                }
              else
                printf(" (pid=%u)", pid);
            }
          putchar('\n');
          continue;
        }
      else if (!names[i])
        /* Ignored or already accumulated. */
        continue;

      printf("  %s:%*c%*d\n",
             names[i], maxlen-strlen(names[i]), ' ',
             maxcount, resources[i].count);
      free(names[i]);
    } /* for */
  free(names);
} /* print_resource_list */

/* Using the xres extension query $win's or all clients' resource usage
 * and print it out nicely formatted. */
static void print_resources(Window win, enum resource_listing_t what)
{
  int foo, nresources;
  XResType *resources;
  XResNClients nclients;
  unsigned long spixmaps;

  nclients = 0;
  XResQueryExtension(Dpy, &foo, &foo);
  if (what == SINGLE_CLIENT)
    {
      XResQueryClientPixmapBytes(Dpy, win, &spixmaps);
      XResQueryClientResources(Dpy, win, &nresources, &resources);
    }
  else
    {
      XResNClients i;
      XResClientIdSpec spec;
      XResClientIdValue *clients;

      /*
       * Query what clients does the server have and the resources
       * of each of them.  We also want to know the PIDs.  $resources
       * will store the uncumulated list of resources in the order of
       * $clients.  If we need to print resource usage of EACH_CLIENT
       * the clients' elements are separated by some meta and pseudo
       * elements.
       */
      spixmaps    = 0;
      nresources  = 0;
      resources   = NULL;
      spec.mask   = XRES_CLIENT_ID_PID_MASK;
      spec.client = None;
      XResQueryClientIds(Dpy, 1, &spec, &nclients, &clients);
      for (i = 0; i < nclients; i++)
        {
          XID client;
          XResType *itsres;
          int nitsres, plus;
          unsigned long itspixmaps;

          client = XRESCLIENT(clients, i);

          /* $itsres := this $client's resources */
          XResQueryClientPixmapBytes(Dpy, client, &itspixmaps);
          XResQueryClientResources(Dpy, client, &nitsres, &itsres);
          spixmaps += itspixmaps;

          /* Ensure $resources is large enough to contain $itsres too. */
          plus = nitsres;
          if (what == EACH_CLIENT)
            /* We'll want to add three more elements
             * in addition to $itsres. */
            plus += 3;
          xrealloc(&resources, sizeof(*resources)*(nresources+plus));

          /* Add extra elements before $itsres. */
          if (what == EACH_CLIENT)
            { /* Client XID and the number of elements in $resources
               * pertaining to it. */
              resources[nresources].resource_type = XRESCLIENT(clients, i);
              resources[nresources].count = plus;
              nresources++;

              /* PID of the client. */
              resources[nresources].resource_type = 0;
              resources[nresources].count = XResGetClientPid(&clients[i]);
              nresources++;

              /* XResQueryClientPixmapBytes() of this client. */
              resources[nresources].resource_type =
                XInternAtom(Dpy, "pixmap bytes", False);
              resources[nresources].count = itspixmaps;
              nresources++;
            }

          /* Append $itsres to $resources. */
          memcpy(&resources[nresources], itsres, sizeof(*itsres)*nitsres);
          nresources += nitsres;
          XFree(itsres);
        }
      XResClientIdsDestroy(nclients, clients);
    } /* if */

  /* Print $resources and bye. */
  print_resource_list(what, nresources, resources, nclients, spixmaps);
  if (what == SINGLE_CLIENT)
    XFree(resources);
  else
    free(resources);
} /* print_resources */
#endif /* HAVE_XRES */

/* Image file writing (-z) */
struct image_st
{
  FILE *st;
  Bool has_alpha;

#ifdef HAVE_GDK_PIXBUF
  char *tfname;
  char const *fname, *fmt;
  guchar *ptr;
  GdkPixbuf *pixbuf;
#endif

#ifdef HAVE_QT
  QRgb *ptr;
  QImage *qimg;
  QImageWriter *writer;
#endif
};

#ifdef HAVE_QT
# define mkrgb qRgba
typedef QRgb rgb_st;
#else
typedef struct
{
  unsigned char r, g, b, a;
} rgb_st;

static rgb_st mkrgb(unsigned char r, unsigned char g, unsigned char b,
                    unsigned char a)
{
  rgb_st rgb;

  rgb.r = r;
  rgb.g = g;
  rgb.b = b;
  rgb.a = a;

  return rgb;
} /* mkrgb */
#endif /* ! HAVE_QT */

/* Returns the length of $n in decimal representation. */
static unsigned digitsof(unsigned long n)
{
  unsigned d;

  d = 0;
  do
    {
      d++;
      n /= 10;
    }
  while (n);

  return d;
} /* digitsof */

/*
 * Parses a "/yaddala-%c.png/"-style template and returns the string with
 * all tokens expanded, or NULL if $str is not a template.  The recognized
 * tokens are: %Y, %M, %D (year, month, day), %h, %m, %s (hour, min, sec),
 * %S, %u (Unix time, microseconds), %t, %T (shorthands), %[<n>]c and %<n>C
 * (counters).
 */
static char *fname_template(char const *str)
{
  static unsigned cnt;
  char const *in;
  char *out, *ret;
  struct timeval tv;
  struct tm const *tm;
  unsigned tmpcnt, lout;

  /* Is this a template? */
  if (!(str[0] == '/' && str[1] && str[strlen(str)-1] == '/'))
    return NULL;

  /* Parse what's inside the // and see how much space we need. */
  str++;
  tm = NULL;
  tmpcnt = cnt;
  for (in = str, lout = 0; !(in[0] == '/' && !in[1]); )
    {
      char c;
      unsigned minwidth, i;

      /* Special character? */
      if (*in++ != '%')
        {
          lout++;
          continue;
        }

      /* See what to substitute. */
      i = 1;
      if (strspn(in, "YMDhmsuStT"))
        { /* Time.  Get it if we haven't. */
          if (!tm)
            {
              time_t t;

              gettimeofday(&tv, NULL);
              t = tv.tv_sec;
              tm = localtime(&t);
            }

          if (*in == 't')
            /* YYYY-MM-DD_hh:mm:dd */
            lout += ((4+2+2)+2 + (2+2+2)+2) + 1;
          else if (*in == 'T')
            /* epoch.ms */
            lout += digitsof(tv.tv_sec) + 1 + 6;
          else if (*in == 'S')
            lout += digitsof(tv.tv_sec);
          else if (*in == 'u')
            lout += 6;
          else if (*in == 'Y')
            lout += 4;
          else /* MM, DD, hh, mm, ss */
            lout += 2;
        }
      else if (*in == 'c')
        /* The counter. */
        lout += digitsof(tmpcnt++);
      else if (sscanf(in, "%u%c%n", &minwidth, &c, &i) >= 2)
        { /* The counter, in a specified width. */
          if (c == 'c')
            { /* Do increment the counter in order to be
               * up to date about its decimal length. */
              lout += larger(minwidth, digitsof(tmpcnt++));
            }
          else if (c == 'C')
            { /* Always print as many digits as the counter
               * can maximally have. */
              if (minwidth < 1)
                die("bad prec\n");
              lout += digitsof(minwidth-1);
            }
          else
            die("syntax error\n");
        } else if (*in == '%')
          lout++;
      else
        die("syntax error\n");

      /* Skip as many characters as we've parsed. */
      in += i;
    } /* until terminator is reached */

  /* Go through the strings again and construct the output for real. */
  in = str;
  ret = xmalloc(&out, ++lout);
  while (lout > 1)
    {
      char c;
      unsigned i, o, minwidth;

      if (*in != '%')
        {
          lout--;
          *out++ = *in++;
          continue;
        } else /* Skip the '%'. */
          in++;

      /* Expand */
      i = o = 1;
      if (*in == 'Y')
        o = snprintf(out, lout, "%.4u", 1900+tm->tm_year);
      else if (*in == 'M')
        o = snprintf(out, lout, "%.2u", 1+tm->tm_mon);
      else if (*in == 'D')
        o = snprintf(out, lout, "%.2u", tm->tm_mday);
      else if (*in == 'h')
        o = snprintf(out, lout, "%.2u", tm->tm_hour);
      else if (*in == 'm')
        o = snprintf(out, lout, "%.2u", tm->tm_min);
      else if (*in == 's')
        o = snprintf(out, lout, "%.2u", tm->tm_sec);
      else if (*in == 's')
        o = snprintf(out, lout, "%.2u", tm->tm_sec);
      else if (*in == 'u')
        o = snprintf(out, lout, "%.6lu", tv.tv_usec);
      else if (*in == 't')
        o = snprintf(out, lout,
                     "%.4u-%.2u-%.2u_%.2u:%.2u:%.2u",
                     1900+tm->tm_year, 1+tm->tm_mon, tm->tm_mday,
                     tm->tm_hour, tm->tm_min, tm->tm_sec);
      else if (*in == 'S')
        o = snprintf(out, lout, "%lu", tv.tv_sec);
      else if (*in == 'T')
        o = snprintf(out, lout, "%lu.%.6lu", tv.tv_sec, tv.tv_usec);
      else if (*in == 'c')
        o = snprintf(out, lout, "%u", cnt++);
      else if (sscanf(in, "%u%c%n", &minwidth, &c, &i) >= 2)
        {
          if (c == 'c')
            o = snprintf(out, lout, "%0*u", minwidth, cnt++);
          else if (c == 'C')
            {
              o = snprintf(out, lout, "%0*u", digitsof(minwidth-1), cnt++);
              cnt %= minwidth;
            }
        }
      else
        {
          assert(*in == '%');
          *out = *in;
        }

      /* Proceed */
      in   += i;
      out  += o;
      lout -= o;
    } /* until finished output */

  /* Verify that we've consumed all input (and didn't overrun). */
  assert(in[0] == '/' && !in[1]);
  *out = '\0';

  return ret;
} /* fname_template */

#ifdef HAVE_GDK_PIXBUF
/* Returns the GdkPixbufFormat::name of the format $fname should be
 * saved in looking at its extension, or NULL to go with raw RGB. */
static char const *determine_output_format(char const *fname)
{
  char const *ext;
  GSList *formats, *li;
  GdkPixbufFormat *format;

  if (!(ext = strrchr(fname, '.')))
    return NULL;
  ext++;

  formats = gdk_pixbuf_get_formats();
  for (li = formats; format = NULL, li; li = li->next)
    {
      unsigned i;
      gboolean match;
      char **extensions;

      format = (GdkPixbufFormat *)li->data;
      if (!gdk_pixbuf_format_is_writable(format))
        continue;
      if (gdk_pixbuf_format_is_disabled(format))
        continue;

      match = FALSE;
      extensions = gdk_pixbuf_format_get_extensions(format);
      for (i = 0; extensions[i] && !match; i++)
        match = !strcmp(ext, extensions[i]);
      g_strfreev(extensions);
      if (match)
        break;
    } /* for known formats */

  g_slist_free(formats);
  return format ? gdk_pixbuf_format_get_name(format) : NULL;
} /* determine_output_format */
#endif /* HAVE_GDK_PIXBUF */

/* Prepare writing an image to $fname. */
static void open_image(struct image_st *img, char const *fname,
                       unsigned width, unsigned height, Bool has_alpha)
{
  static __attribute__((unused)) int warned;
  char *tfname;
  memset(img, 0, sizeof(*img));
  img->has_alpha = has_alpha;

  /* If $fname is a template, use the expanded form. */
  if ((tfname = fname_template(fname)) != NULL)
    fname = tfname;
  if (Verbose)
    printf("saving image to %s\n", fname);

#ifdef HAVE_GDK_PIXBUF
  if ((img->fmt = determine_output_format(fname)) != NULL)
    {
      g_type_init();
      img->fname = fname;
      img->tfname = tfname;
      img->pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, has_alpha, 8,
                                   width, height);
      img->ptr = (guchar *)gdk_pixbuf_get_pixels(img->pixbuf);
      return;
    }
#endif /* HAVE_GDK_PIXBUF */

#ifdef HAVE_QT
  img->writer = new QImageWriter(fname);
  if (img->writer->canWrite())
    {
      img->qimg = new QImage(width, height, has_alpha
                             ? QImage::Format_ARGB32
                             : QImage::Format_RGB32);
      img->ptr = (QRgb *)img->qimg->bits();
      free(tfname);
      return;
    }
  else if (img->writer->error() == QImageWriter::UnsupportedFormatError)
    { /* Fallback */
      delete img->writer;
      img->writer = NULL;
    }
  else
    die("cannot write image\n");
#endif /* HAVE_QT */

  /* Write straight RGB.  If this is the only format we support (because of
   * the lack of toolkits), warn the user the first time we emit an image. */
#if !defined(HAVE_GDK_PIXBUF) && !defined(HAVE_QT)
  if (!warned)
    {
      fputs("Warning: writing raw RGB image.  You can convert it to PNG "
            "by the following ImageMagick command:\n", stderr);
      warned = 1;
    }
#endif /* ! HAVE_GDK_PIXBUF && ! HAVE_QT */
  if (!(img->st = fopen(fname, "w")))
    die("couldn't open output file\n");
  printf("convert -size %ux%u -depth 8 %s:'%s' '%s.png';\n",
         width, height, has_alpha ? "rgba" : "rgb",
         fname, fname);
  fflush(stdout);
  free(tfname);
} /* open_image */

/* Add an $rgb to $img. */
static void write_image(struct image_st *img, rgb_st rgb)
{
#ifdef HAVE_GDK_PIXBUF
  if (img->pixbuf)
    {
      *img->ptr++ = rgb.r;
      *img->ptr++ = rgb.g;
      *img->ptr++ = rgb.b;
      if (img->has_alpha)
        *img->ptr++ = rgb.a;
      return;
    }
#endif /* HAVE_GDK_PIXBUF */

#ifdef HAVE_QT
  if (img->qimg)
    {
      *img->ptr++ = rgb;
      return;
    }
#endif /* HAVE_QT */

  fwrite(&rgb, img->has_alpha ? 4 : 3, 1, img->st);
} /* write_image */

/* Finalize $img. */
static void close_image(struct image_st *img)
{
#ifdef HAVE_GDK_PIXBUF
  if (img->pixbuf)
    {
      if (!gdk_pixbuf_save(img->pixbuf, img->fname, img->fmt, NULL, NULL))
        die("failed to save image\n");
      gdk_pixbuf_unref(img->pixbuf);
      free(img->tfname);
      return;
    }
#endif /* HAVE_GDK_PIXBUF */

#ifdef HAVE_QT
  if (img->qimg)
    {
      if (!img->writer->write(*img->qimg))
        die("failed to save image\n");
      delete img->qimg;
      delete img->writer;
      return;
    }
#endif /* HAVE_QT */

  /* Written a plain file. */
  if (fclose(img->st))
    die("write error\n");
} /* close_image */

/* Convert an XImage::data-like byte array to plain RGB888 and write it
 * to $fname. */
static void save_rgb_image(char const *fname, unsigned char const *in,
                           unsigned width, unsigned height,
                           unsigned depth, unsigned bpp, unsigned row,
                           unsigned red, unsigned green, unsigned blue)
{
  unsigned alpha;
  struct image_st out;
  unsigned inner, outer;

  /* Assume there's an alpha channel if the color masks don't cover
   * the full $depth.  Be pedantic about not overflowing if it's 32. */
  assert(red && green && blue);
  alpha = ((((1 << (depth-1)) - 1) << 1) | 1) & ~(red|green|blue);

  /* If $Rotated start scanning from bottom-left and traverse up/right. */
  assert(width > 0 && height > 0);
  if (Rotated)
    {
      outer = width;
      inner = height;
      in += row*(height-1);
    }
  else
    {
      outer = height;
      inner = width;
    }

  open_image(&out, fname, inner, outer, !!alpha);
  for (; outer > 0; outer--)
    {
      unsigned i;
      unsigned char const *before, *p, *after;

      for (before = p = after = in, i = inner; i > 0; i--)
        {
          unsigned px, o;

          before = p;
          for (px = o = 0; o < bpp; o += 8)
            px |= *p++ << o;
          write_image(&out,
                      mkrgb(mask_pixel(px, red),
                            mask_pixel(px, green),
                            mask_pixel(px, blue),
                            alpha ? mask_pixel(px, alpha) : 0xff));

          /* $after will be right to $before. */
          after = p;
          if (Rotated)
            /* Back and up */
            p = before - row;
        } /* for columns */

      if (Rotated)
        /* Go to the bottom. */
        in = after + row*(height-1);
      else
        in += row;
    } /* for rows */
  close_image(&out);
} /* save_rgb_image */

/* Y'UV -> RGB, stolen from Wikipedia. */
static rgb_st yuv2rgb(int y, int u, int v)
{
  int c, d, e;

  c = y -  16;
  d = u - 128;
  e = v - 128;

  return mkrgb(clamp((298*c + 409*e         + 128) >> 8, 0, 255),
               clamp((298*c + 100*d - 208*e + 128) >> 8, 0, 255),
               clamp((298*c + 516*d         + 128) >> 8, 0, 255),
               0xff);
} /* yuv2rgb */

static void save_yuv_image(char const *fname, unsigned char const *img,
                           unsigned row, unsigned width, unsigned height)
{
  unsigned i, o;
  struct image_st out;
  struct yuv_st
    {
      unsigned u: 8;
      unsigned y1:8;
      unsigned v: 8;
      unsigned y2:8;
    } const *in;

  /* Each UYVY encodes two pixels. */
  in = (struct yuv_st const *)img;
  open_image(&out, fname, width, height, False);
  for (i = width*height / 2, o = width*2; i > 0; i--, o--, in++)
    {
      if (o == 0)
        { /* End of line */
          o = width*2;
          img  = (unsigned char const *)in;
          img += row - sizeof(*in)*width;
          in = (struct yuv_st const *)img;
        }

      write_image(&out, yuv2rgb(in->y1, in->u, in->v));
      write_image(&out, yuv2rgb(in->y2, in->u, in->v));
    } /* while we have UYVY:s */
  close_image(&out);
} /* save_yuv_image */

/* Return the integer of the specified $width pointed to be $p. */
static long xval2long(void const *p, unsigned width)
{
  if (width == 4)
    return *(long const *)p;
  else if (width == 2)
    return *(short const *)p;
  else if (width == 1)
    return *(char const *)p;
  else
    assert(0);
} /* xval2long */

/* Returns the value(s) of $win's $key or NULL if the window doesn't have
 * such property. */
static char *get_properties(Atom win, Atom key, Atom type,
                            unsigned *nvalsp, unsigned *widthp)
{
  int rfmt;
  Atom rtype;
  unsigned char *val;
  unsigned long n, m;

  if (XGetWindowProperty(Dpy, win, key,
                         0,       /* offset               */
                         -1,      /* length               */
                         False,   /* delete               */
                         type,    /* req_type             */
                         &rtype,  /* actual_type          */
                         &rfmt,   /* actual_format        */
                         &n, &m,  /* nitems, bytes_after  */
                         &val) || !rtype)
    return NULL;

  if (rtype != type)
    die("property has a different type\n");
  assert(rfmt == 8 || rfmt == 16 || rfmt == 32);
  *widthp = rfmt/8;
  *nvalsp = n;
  return (char *)val;
} /* get_properties */

/* Same, but indicate that you're only interested in the first value.
 * The char * return type is practical if you expect a string. */
static char *get_property(Atom win, Atom key, Atom type)
{
  unsigned nvals, width;
  return get_properties(win, key, type, &nvals, &width);
} /* get_property */

/* Pretty-print all values of $key.  It actually prints only one property,
 * the plural marker is for the possible multiple values. */
static void print_properties(Window win, Atom key, Atom type)
{
  char separator;
  char const *curp;
  char *keys, *vals;
  unsigned nvals, width, i;

  keys = XGetAtomName(Dpy, key);
  if (!(vals = get_properties(win, key, type, &nvals, &width)))
    {
      printf("%s: window doesn't have such property\n", keys);
      goto out0;
    }

  /* Separate multiple values with commas if we're interactive or spaces. */
  if (Is_interactive)
    {
      printf("%s=", keys);
      separator = ',';
    }
  else
    separator = ' ';

  if (IS_STRING(type))
    { /* The members of string arrays are just put one after the other. */
      if (nvals > 0 && !vals[nvals-1])
        nvals--;
      for (i = 0; i < nvals; i++)
        putchar(vals[i] ? vals[i] : separator);
      putchar('\n');
      goto out1;
    }

  /* Decode an array of $type/$width somethings. */
  for (i = 0, curp = vals; i < nvals; i++, curp += width)
    {
      long vali;

      if (i > 0)
        putchar(separator);
      vali = xval2long(curp, width);
      if (type == XA_INTEGER)
          printf("%ld", vali);
      else if (type == XA_CARDINAL)
          printf("%lu", vali);
      else if (type == XA_ATOM)
        {
          if (vali)
            {
              char *name;

              name = XGetAtomName(Dpy, vali);
              fputs(name, stdout);
              XFree(name);
            }
          else
            puts("None");
        }
      else
        { /* Assume something XA_WINDOW-like. */
          if (vali)
            printf("0x%lx", vali);
          else
            puts("None");
        }
    } /* for each value */
  putchar('\n');

out1:
  XFree(vals);
out0:
  XFree(keys);
} /* print_properties */

/* Wrap XChangeProperty().  $val must either be a char* or a long*. */
static void set_properties(Atom win, Atom key, Atom type,
                           void const *val, unsigned nvals, int mode)
{
  if (IS_STRING(type))
    { /* Strings in arrays are separated by NUL:s. */
      if (mode == PropModePrepend || mode == PropModeAppend)
        XChangeProperty(Dpy, win, key, type, 8, mode,
                        (unsigned char const *)"", 1);
      XChangeProperty(Dpy, win, key, type, 8, mode,
                      (unsigned char const *)val, strlen((char const *)val));
    }
  else
    XChangeProperty(Dpy, win, key, type, 32, mode,
                    (unsigned char const *)val, nvals);
} /* set_properties */

/* Process a -iIwas command. */
static void process_iiwas(Window win, Atom type, char const *vals,
                          enum propcmds propcmd)
{
  int mode;
  Atom key;
  long *newvals;
  unsigned nvals;

  /* Just print the value of $key? */
  if (!(vals = get_key_val(&key, vals)))
    {
      print_properties(win, key, type);
      return;
    }

  /* Get what we'll (tentatively) pass to set_properties(). */
  nvals = 1;
  if (!IS_STRING(type))
    {
      unsigned i;

      for (i = 0; vals[i]; i++)
        if (vals[i] == ',')
          nvals++;
      xmalloc(&newvals, sizeof(*newvals) * nvals);
      nvals = get_int_list(newvals, nvals, vals);
      vals = (char const *)newvals;
    }
  else
    newvals = NULL;

  mode = PropModeReplace;
  if (propcmd == TOGGLE)
    { /* Either set $key=$val or $key=0. */
      char *cur;
      unsigned n, w;

      if ((cur = get_properties(win, key, type, &n, &w)) != NULL)
        {
          if (!xval2long(cur, w))
            {
              static long nada = 0;
              vals  = (char const *)&nada;
              nvals = 1;
            }
          XFree(cur);
        }
    }
  else if (propcmd == FLIP)
    { /* Either set $key=$val or delete $key. */
      char *cur;

      if ((cur = get_property(win, key, type)) != NULL)
        {
          XFree(cur);
          XDeleteProperty(Dpy, win, key);
          return;
        }
    }
  else if (propcmd == PREPEND)
    mode = PropModePrepend;
  else if (propcmd == APPEND)
    mode = PropModeAppend;

  set_properties(win, key, type, vals, nvals, mode);
  free(newvals);
} /* process_iiwas */

/* Sends a WM_DELETE_WINDOW request to $win */
static void delete_window(Window win)
{
  XEvent ev;

  memset(&ev, 0, sizeof(ev));
  ev.xclient.type = ClientMessage;
  ev.xclient.window = win;
  ev.xclient.message_type = XInternAtom(Dpy, "WM_PROTOCOLS", False);
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = XInternAtom(Dpy, "WM_DELETE_WINDOW", False);
  ev.xclient.data.l[1] = CurrentTime;

  XSendEvent(Dpy, win, False, NoEventMask, &ev);
} /* delete_window */

/* Send a ClientMessage to $dst about $win. */
static void send_client_message(Window dst, Window win, char const *msg)
{
  XEvent ev;

  ev.type = ClientMessage;
  ev.xclient.format = 32;

  ev.xclient.window = win;
  get_int_list(ev.xclient.data.l, MEMBS_OF(ev.xclient.data.l),
               get_key_val(&ev.xclient.message_type, msg));
  XSendEvent(Dpy, dst, False, SubstructureNotifyMask, &ev);
} /* send_client_message */

#ifdef HAVE_XTST
# ifdef HAVE_XI
/* Enumerate all the XInput devices and return the first XDevice for $use. */
static XDevice *find_xinput(int use, unsigned *naxesp)
{
  int ndevs, i;
  XDevice *dev;
  XDeviceInfo *devs, *devi, *xtst;

  dev = NULL;
  devi = xtst = NULL;
  if ((devs = XListInputDevices(Dpy, &ndevs)) != NULL)
    {
      for (i = 0; i < ndevs; i++)
        if (devs[i].use == IsXExtensionPointer)
          {
            devi = &devs[i];
            if (strstr(devi->name, "XTEST"))
              /* Fall back to $xtst if we can't find another pointer. */
              xtst = devi;
            else if ((dev = XOpenDevice(Dpy, devs[i].id)) != NULL)
              break;
          }
    }

  if (!dev && xtst)
    dev = XOpenDevice(Dpy, xtst->id);

#ifndef HAVE_XI2
  /* Count the number of axes $dev has unless we have XI2,
   * in which case the caller does it. */
  if (dev && naxesp)
    {
      int i;
      char const *any;

      for (i = 0, any = (char const *)devi->inputclassinfo, *naxesp = 0;
           i < devi->num_classes;
           i++, any += ((XAnyClassPtr)any)->length)
        if (((XAnyClassPtr)any)->c_class == ValuatorClass)
          *naxesp += ((XValuatorInfoPtr)any)->num_axes;
    }
#endif /* ! HAVE_XI2 */

  XFreeDeviceList(devs);
  return dev;
} /* find_xinput */
# endif /* HAVE_XI */

/* Parses a list of "<modifer>-"...s and fakes keypresses or releases
 * for them.  Returns the start of the real keysym. */
static char const *send_modifiers(char const *keystr, Bool press, Time t)
{
  static struct { char const *mod; KeySym sym; } const mods[] =
  {
    { "shift-", XK_Shift_L }, { "ctrl-", XK_Control_L        },
    { "alt-",   XK_Alt_L   }, { "fn-",   XK_ISO_Level3_Shift },
  };
  unsigned i;
  char const *rest;

  i = 0;
  do
    if ((rest = isprefix(keystr, mods[i].mod)) != NULL)
      {
        keystr = rest;
        XTestFakeKeyEvent(Dpy, XKeysymToKeycode(Dpy, mods[i].sym),
                          press, t);
        i = 0;
      }
    else
      i++;
  while (i < MEMBS_OF(mods));

  return keystr;
} /* send_modifiers */
#endif /* HAVE_XTST */

/* Synthetize ButtonPress/ButtonRelease/Motion pointer event. */
static void pointer_event(Window win,
                          int what, XPoint const *where,
                          int delay, Bool xtst)
{
  static int state;

  if (xtst)
    { /* Fake a pointer event. */
#ifdef HAVE_XTST
# ifdef HAVE_XI
      static XDevice *xinput;
      static Bool xinput_checked = False;

      /* Which axis mean what for $xinput. */
      enum
        { /* Exactly what "touch major/minor" mean is a good question,
           * but they are necessary to set in Harmattan, otherwise we
           * cannot pan in the applications because Qt ignores us. */
          XINPUT_AXIS_POS_X, XINPUT_AXIS_POS_Y,
          XINPUT_AXIS_TOUCH_MAJOR, XINPUT_AXIS_TOUCH_MINOR,
        };
      static struct { char const *name; unsigned idx; } axis_map[] =
        { /* name -> index */
          { "Abs MT Position X"   }, /* XINPUT_AXIS_POS_X       */
          { "Abs MT Position Y"   }, /* XINPUT_AXIS_POS_Y       */
          { "Abs MT Touch Major"  }, /* XINPUT_AXIS_TOUCH_MAJOR */
          { "Abs MT Touch Minor"  }, /* XINPUT_AXIS_TOUCH_MINOR */
          { "Abs MT Tracking ID"  },
        };
      static unsigned naxes;

      /* Fake xinput event if possible. */
      if (!xinput_checked)
        {
          xinput = find_xinput(IsXExtensionPointer, &naxes);
          xinput_checked = True;

          /* Figure out which axis means what and fill out @axis_map. */
          if (xinput)
            {
#  ifdef HAVE_XI2
              int i;
              XIDeviceInfo *devi;

              devi = XIQueryDevice(Dpy, xinput->device_id, &i);
              if (i > 0)
                {
                  unsigned axis, known_axes;

                  axis = known_axes = 0;
                  for (i = 0;
                       known_axes < MEMBS_OF(axis_map)
                             && i < devi->num_classes;
                       i++)
                    {
                      char *name;
                      unsigned o;

                      if (devi->classes[i]->type != XIValuatorClass)
                        continue;
                      axis++;

                      name = XGetAtomName(Dpy,
                        ((XIValuatorClassInfo*)devi->classes[i])->label);

                      for (o = 0; o < MEMBS_OF(axis_map); o++)
                        if (!axis_map[o].idx
                            && !strcmp(axis_map[o].name, name))
                          {
                            known_axes++;
                            naxes = axis_map[o].idx = axis;
                            break;
                          }
                      XFree(name);
                    }
                }
              XIFreeDeviceInfo(devi);
#  else /* ! HAVE_XI2 */
              if (naxes >= 2)
                { /* No information, assume that the first two axes
                   * are the position. */
                  naxes = 2;
                  axis_map[XINPUT_AXIS_POS_X].idx = 1;
                  axis_map[XINPUT_AXIS_POS_Y].idx = 2;
                }
#  endif /* ! HAVE_XI2 */

              if (   !axis_map[XINPUT_AXIS_POS_X].idx
                  || !axis_map[XINPUT_AXIS_POS_Y].idx)
                { /* Unusable */
                  XCloseDevice(Dpy, xinput);
                  xinput = NULL;
                }
            }
        }
      if (xinput)
        {
          int *axes;

          xmalloc(&axes, sizeof(*axes) * naxes);
          memset(axes, 0, sizeof(*axes)* naxes);

          assert(   axis_map[XINPUT_AXIS_POS_X].idx
                 && axis_map[XINPUT_AXIS_POS_Y].idx);
          axes[axis_map[XINPUT_AXIS_POS_X].idx-1] = where->x;
          axes[axis_map[XINPUT_AXIS_POS_Y].idx-1] = where->y;

          if (what == MotionNotify)
            {
              if (axis_map[XINPUT_AXIS_TOUCH_MAJOR].idx)
                axes[axis_map[XINPUT_AXIS_TOUCH_MAJOR].idx-1] = 45;
              if (axis_map[XINPUT_AXIS_TOUCH_MINOR].idx)
                axes[axis_map[XINPUT_AXIS_TOUCH_MINOR].idx-1] = 45;
              XTestFakeDeviceMotionEvent(Dpy, xinput, False, 0,
                                         axes, naxes, delay);
            }
          else if (what == ButtonPress)
            {
              if (axis_map[XINPUT_AXIS_TOUCH_MAJOR].idx)
                axes[axis_map[XINPUT_AXIS_TOUCH_MAJOR].idx-1] = 30;
              if (axis_map[XINPUT_AXIS_TOUCH_MINOR].idx)
                axes[axis_map[XINPUT_AXIS_TOUCH_MINOR].idx-1] = 30;
              XTestFakeDeviceButtonEvent(Dpy, xinput, Button1, True,
                                         axes, naxes, delay);
            }
          else if (what == ButtonRelease)
            XTestFakeDeviceButtonEvent(Dpy, xinput, Button1, False,
                                       axes, 0, delay);
          free(axes);
        }
      else
# endif /* HAVE_XI */
        {
          if (what == MotionNotify)
            XTestFakeMotionEvent(Dpy, Scr, where->x, where->y, delay);
          else if (what == ButtonPress)
            XTestFakeButtonEvent(Dpy, Button1, True, delay);
          else if (what == ButtonRelease)
            XTestFakeButtonEvent(Dpy, Button1, False, delay);
        }
#else /* ! HAVE_XTST */
      die("no xtst\n");
#endif
    }
  else
    { /* Send a synthetic pointer event. */
      XEvent ev;

      memset(&ev, 0, sizeof(ev));
      if (what == MotionNotify)
        {
          assert(XTranslateCoordinates(Dpy, Root, win,
                                       where->x, where->y,
                                       &ev.xmotion.x, &ev.xmotion.y,
                                       &ev.xmotion.subwindow));
          ev.xmotion.x_root = where->x;
          ev.xmotion.y_root = where->y;
          ev.xmotion.time   = delay;
          ev.xmotion.root   = Root;
          ev.xmotion.window = win;
          ev.xmotion.state  = state;
        }
      else
        {
          assert(XTranslateCoordinates(Dpy, Root, win,
                                       where->x, where->y,
                                       &ev.xbutton.x, &ev.xbutton.y,
                                       &ev.xbutton.subwindow));
          ev.xbutton.x_root = where->x;
          ev.xbutton.y_root = where->y;
          ev.xbutton.time   = delay;
          ev.xbutton.root   = Root;
          ev.xbutton.window = win;
          ev.xbutton.state  = state;
          ev.xbutton.button = Button1;
        }

      ev.type = what;
      XSendEvent(Dpy, win, True, NoEventMask, &ev);
    } /* if */

  /* Remember the state we faked for the next event. */
  if (what == ButtonPress)
    state |=  Button1Mask;
  else if (what == ButtonRelease)
    state &= ~Button1Mask;
} /* pointer_event */

/* Validate a -X app/mapp/desktop command. */
static char const *ismkapwin(char const *str)
{
  char const *p;

  if ((p = isprefix(str,      "app")) != NULL)
    return p;
  if ((p = isprefix(str,     "uapp")) != NULL)
    return p;
  if ((p = isprefix(str,     "mapp")) != NULL)
    return p;
  if ((p = isprefix(str,    "umapp")) != NULL)
    return p;
  if ((p = isprefix(str,  "desktop")) != NULL)
    return p;
  if ((p = isprefix(str, "udesktop")) != NULL)
    return p;
  return NULL;
} /* ismkapwin */

/* Execute $ncmd commands from $argv on $win.  $implicit_win tells whether
 * $win was not given explicitly, so the command substitute it with something
 * else that make more sense for that command. */
static Window command_block(int argc, char const *const *argv, unsigned ncmds,
                            Window win, Bool implicit_win)
{
  int opt_Q;
  enum propcmds propcmd;

  /* opt_Q   := how many -Q preceeded a command
   * propcmd := what -p flag was specified before the command */
  opt_Q = 0;
  propcmd = NONE;
  for (; ncmds > 0; ncmds--)
    {
      int optchar;

      /* Preprocess the command. */
      optchar = getopt(argc, (char **)argv, Optstring);
      switch (optchar)
        {
          /* These are already processed by main(). */
          case 'v':
            continue;
          case 'W':
            {
              int n;

              if (!strcmp(optarg, "do"))
                      continue;
              if (!strcmp(optarg, "repeat")
                  || sscanf(optarg, "repeat=%d", &n) == 1)
                      continue;
              if (!strcmp(optarg, "loop")
                  || sscanf(optarg, "loop=%d", &n) == 1)
                      continue;
              if (!strcmp(optarg, "quit") || !strcmp(optarg, "exit"))
                      continue;
              break;
            }

          /* Change processing of the next property? */
          case 'p':
            if (!strcmp(optarg, "prepend"))
              propcmd = PREPEND;
            else if (!strcmp(optarg, "append"))
              propcmd = APPEND;
            else if (!strcmp(optarg, "toggle"))
              propcmd = TOGGLE;
            else if (!strcmp(optarg, "flip"))
              propcmd = FLIP;
            else
              die("unknown property command\n");
            continue;

          /* Don't let -Q affect an immediately preceeding -p. */
          case 'Q':
            opt_Q++;
            continue;
        } /* preprocess */

      /* Do what we're asked to do to $win. */
      switch (optchar)
        {
          /* Return information about a window. */
          case 'q':
            XSync(Dpy, False);
            print_info(win, opt_Q, 0);
            break;

#ifdef HAVE_XRES
          /* Show $win's X resource usage. */
          case 'r':
            /* -Q:  show all windows' resources
             * -QQ: show cumularive resource usage */
            print_resources(win,
                              opt_Q == 0 ? SINGLE_CLIENT
                            : opt_Q == 1 ? EACH_CLIENT
                                         : SYSTEMWIDE);
            break;
#endif /* HAVE_XRES */

          /* Save a screenshot in a file. */
          case 'z':
            if (!opt_Q)
              { /* Take a screenshot of $win. */
                XImage *img;
                Visual visual;
                XWindowAttributes attrs;

                /*
                 * Get the maximal rectangle in $win we can take
                 * the image of.  For pixmaps we can get it all,
                 * but for windows, only those regions have possibly
                 * have backing store which are currently on the
                 * screen, ie. in $DpyWidthx$DpyHeight+0+0.
                 */
                if (get_win_attrs(win, &attrs, True, &visual))
                  {
                    Window w;
                    int x, y;

                    /* Translate to screen coordinates and calculate
                     * the intersecting rectangle with the screen.
                     * Then translate back to $win coordinates. */
                    assert(XTranslateCoordinates(Dpy, win, Root, 0, 0,
                                                 &attrs.x, &attrs.y, &w));
                    attrs.width  = intersect(&x, attrs.x, attrs.width,
                                             DpyWidth);
                    attrs.height = intersect(&y, attrs.y, attrs.height,
                                             DpyHeight);
                    if (!attrs.width || !attrs.height)
                      die("window is out of screen\n");
                    attrs.x -= x;
                    attrs.y -= y;
                  }

                /* Get the $img. */
                assert((img = XGetImage(Dpy, win, attrs.x, attrs.y,
                                        attrs.width, attrs.height,
                                        AllPlanes, ZPixmap)) != NULL);
                if (!img->red_mask)
                  { /* These are left empty for pixmaps. */
                    assert(attrs.visual);
                    img->red_mask   = attrs.visual->red_mask;
                    img->green_mask = attrs.visual->green_mask;
                    img->blue_mask  = attrs.visual->blue_mask;
                  }

                save_rgb_image(optarg, (unsigned char const *)img->data,
                        attrs.width, attrs.height,
                        img->depth, img->bits_per_pixel, img->bytes_per_line,
                        img->red_mask, img->green_mask, img->blue_mask);
                XDestroyImage(img);
              }
            else if (opt_Q == 1)
              { /* Save the current contents of the framebuffer. */
                int hfb;
                unsigned fbn;
                size_t fbsize;
                unsigned char *fbdup;
                unsigned char const *fb;
                char fname[sizeof("/dev/fb0")];
                unsigned width, height, row, bpp, red, green, blue;
                enum { RGB, YUV422 } format;

                /* Which fb to dump?  Ignore $win if it doesn't make sense. */
                fbn = implicit_win || win == Root || win == Newborn ? 0 : win;
                assert(fbn < 10);
                sprintf(fname, "/dev/fb%u", fbn);
                assert((hfb = open(fname, O_RDONLY)) >= 0);

                /* Get the pixel data format. */
                format = RGB;
                  {
#ifdef HAVE_FB
                    struct fb_fix_screeninfo fix;
                    struct fb_var_screeninfo var;

                    assert(ioctl(hfb, FBIOGET_FSCREENINFO, &fix) == 0);
                    assert(ioctl(hfb, FBIOGET_VSCREENINFO, &var) == 0);

                    width  = var.xres;
                    height = var.yres;
                    row    = fix.line_length;

                    bpp   = var.bits_per_pixel;
                    red   = ((1<<var.red.length)   - 1) << var.red.offset;
                    green = ((1<<var.green.length) - 1) << var.green.offset;
                    blue  = ((1<<var.blue.length)  - 1) << var.blue.offset;

                    /* Is it RGB to start with? */
                    switch (var.nonstd)
                      {
                      case 0:
                        /* RGB */
                        break;

# ifdef HAVE_OMAPFB
                      case OMAPFB_COLOR_YUV422:
                        format = YUV422;
                        break;
# endif /* HAVE_OMAPFB */
                      default:
                        die("unknown pixel data format\n");
                      }
#else /* ! HAVE_FB */
                    /* Fuck knows, assume RGB565. */
                    width  = DpyWidth;
                    height = DpyHeight;
                    bpp    = 16;
                    row    = DpyWidth * bpp/8;
                    red    = 0xf800;
                    green  = 0x07e0;
                    blue   = 0x001f;
#endif /* ! HAVE_FB */
                  }

                /* Get out the frame as quickly as possible.
                 * mmap() seems to work better than read(),
                 * because it always gets complete frames. */
                fbsize = row*height;
                xmalloc(&fbdup, fbsize);
                assert((fb = (unsigned char const *)mmap(NULL,
                        fbsize, PROT_READ, MAP_SHARED,
                        hfb, 0)) != MAP_FAILED);
                memcpy(fbdup, fb, fbsize);

                if (format == RGB)
                  save_rgb_image(optarg, fbdup, width, height,
                                 bpp, bpp, row, red, green, blue);
                else if (format == YUV422)
                  save_yuv_image(optarg, fbdup, row, width, height);

                munmap((char *)fb, fbsize);
                close(hfb);
                free(fbdup);
              }
            else if (opt_Q > 1)
              { /* Save an image from an shm. */
#ifdef HAVE_SGX
                pid_t pid;
                struct shm_info t1;
                struct shmid_ds t2;
                Bool find_latest;
                time_t latest_time;
                int maxid, id, shmid, latest;
                Visual visual;
                XWindowAttributes attrs;
                unsigned char const *img;
                unsigned fbbits, sgx_pitch_align, devkind, pitch, shmsize;

                /* Figure out what size of shm:s to look for.  $fbbits is
                 * an internal X server constant, the rest of the voodoo
                 * comes from fbdev. */
                get_win_attrs(win, &attrs, True, &visual);
                fbbits = 5;
                sgx_pitch_align = attrs.width < EURASIA_TAG_STRIDE_THRESHOLD
                  ? EURASIA_TAG_STRIDE_ALIGN0 : EURASIA_TAG_STRIDE_ALIGN1;
                devkind = (attrs.width*attrs.depth + 7) / 8;
                pitch   = roundto(devkind, sgx_pitch_align * attrs.depth/8);
                pitch   = roundto(pitch, (1 << fbbits) / 8);
                shmsize = roundto(pitch * attrs.height, getpagesize());

                /* Find suitable shm:s by enumerating all of them and
                 * checking their size and creator/last accessor. */
                img = NULL;
                find_latest = opt_Q < 3;
                latest = latest_time = 0;
                pid = get_client_pid(win, False);
                maxid = shmctl(0, SHM_INFO, (struct shmid_ds *)&t1);
                for (id = 0; ; id++)
                  {
                    if (id <= maxid)
                      { /* There're shm:s we haven't checked out yet. */
                        struct shmid_ds sbuf;

                        if ((shmid = shmctl(id, SHM_STAT, &t2)) < 0)
                          continue;
                        assert(shmctl(shmid, IPC_STAT, &sbuf) == 0);

                        /*
                         * Is this shm good for us?  Since the shm:s are
                         * created by the server, for clients we have to
                         * rely on the last accessing PID, but if we're
                         * dumping everything we can, we can check the
                         * creator.
                         */
                        if (sbuf.shm_segsz != shmsize)
                          continue;
                        if (!(pid < 0 || sbuf.shm_lpid == pid
                              || (win == Root && sbuf.shm_cpid == pid)))
                          continue;

                        /* Is this shm younger than $latest? */
                        if (find_latest)
                          {
                            if (latest_time < sbuf.shm_atime)
                              {
                                latest_time = sbuf.shm_atime;
                                latest = shmid;
                              }
                            continue;
                          }
                      }
                    else if (find_latest && latest_time)
                      {
                        shmid = latest;
                        printf("latest %d %lu\n", latest, latest_time);
                      }
                    else
                      break;

                    /* Save the shm. */
                    img = (unsigned char const *)shmat(shmid, NULL,
                                                       SHM_RDONLY);
                    assert(img != (unsigned char const *)-1);

                    save_rgb_image(optarg, img,
                            attrs.width, attrs.height,
                            attrs.depth, roundto(attrs.depth, 8), pitch,
                            attrs.visual->red_mask,
                            attrs.visual->green_mask,
                            attrs.visual->blue_mask);
                    shmdt(img);

                    if (find_latest)
                      break;
                  } /* for all shm:s */

                if (!img)
                  die("couldn't find any good shm\n");
#else
                die("feature not available\n");
#endif
              }
            break;

          /* Create a new window. */
          case 'n':
          case 'N': /* ...but don't map it yet */
            {
              char name[24];
              Bool independent;
              XRectangle geo;
              Visual *visual;
              char const *p, *color;
              XSetWindowAttributes attrs;
              unsigned flags, wclass, depth;

              /* Should the newborn window become $win? */
              if ((independent = optarg[0] == '!'))
                optarg++;

              /* Create an override-redirected window? */
              flags  = 0;
              if ((p = isprefix(optarg, "OR=")) != NULL)
                {
                  optarg = (char *)p;
                  attrs.override_redirect = True;
                  flags |= CWOverrideRedirect;
                }

              /* Choose the visual and the background color. */
              wclass = CopyFromParent;
              visual = CopyFromParent;
              depth  = CopyFromParent;
              color  = get_geometry(optarg, &geo);
              if (!*color)
                { /* Choose some background. */
                  attrs.background_pixel = 0x7f7f7f7f;
                  flags |= CWBackPixel;
                }
              else if (!strcasecmp(color, "@input"))
                {
                  wclass = InputOnly;
                }
              else if (strcasecmp(color, "@none"))
                { /* Background specified. */
                  /* Is $depth == 32?  The background color pixel value
                   * depends on the colormap, which depends on $depth. */
                  if (color[0] == '@')
                    { /* Check if @<color-name> is followed by %<alpha>. */
                      unsigned i;

                      /* Assume that <color-name> is full alphanumeric. */
                      for (i = 1; isalnum(color[i]); i++)
                        ;
                      if (color[i] == '%')
                        depth = 32;
                    }
                  else if (color[0] == '#' || color[0] == '%')
                    depth = 32;
                  else
                    die("invalid color specification\n");

                  /* Create a TrueColor colormap if necessary. */
                  if (depth == 32)
                    {
                      XVisualInfo vi;

                      XMatchVisualInfo(Dpy, Scr, depth, TrueColor, &vi);
                      attrs.colormap = XCreateColormap(Dpy, Root,
                                             vi.visual, AllocNone);
                      flags |= CWColormap;
                      visual = vi.visual;
                    }
                  else
                    attrs.colormap = None;

                  /* Get the background color pixel value. */
                  if (*get_color_pixel(attrs.colormap, color,
                                       &attrs.background_pixel) != '\0')
                    die("junk after color specification\n");
                  flags |= CWBackPixel;

                  /* A border color has to be specified too if the visuals
                   * of the window and its parent are different. */
                  if (depth == 32)
                    {
                      attrs.border_pixel = attrs.background_pixel;
                      flags |= CWBorderPixel;
                    }
                }

              Newborn = XCreateWindow(Dpy,
                                      /* parent */
                                      win,
                                      /* geometry */
                                      geo.x, geo.y, geo.width, geo.height,
                                      /* border_width, depth */
                                      0, depth,
                                      /* class, visual */
                                      wclass, visual,
                                      /* attributes */
                                      flags, &attrs);
              if (Verbose)
                printf("New window: 0x%lx\n", Newborn);

              /* Give a default unique name to the window. */
              snprintf(name, sizeof(name), "map_%u_%d", NWindows++, getpid());
              XStoreName(Dpy, Newborn, name);

              if (optchar == 'n')
                XMapWindow(Dpy, Newborn);
              if (!independent)
                {
                  win = Newborn;
                  implicit_win = False;
                }
              break;
            }

          /* Change the window geometry. */
          case 'g':
            {
              char const *p;
              XRectangle rect;

              if ((p = isprefix(optarg, "shape=")) != NULL
                  || (p = isprefix(optarg, "clip=")) != NULL
                  || (p = isprefix(optarg, "input=")) != NULL)
                { /* Change the window shape. */
#ifdef HAVE_XFIXES
                  int kind;
                  XserverRegion region, rgrect;
                  enum { UNION, INTERSECT, SUBTRACT } setop;

                  /* Which shape to change? */
                  kind = optarg[0] == 's' ? ShapeBounding
                       : optarg[0] == 'c' ? ShapeClip
                       :                    ShapeInput;
                  optarg = (char *)p;

                  /* Remove shape? */
                  if (!strcmp(optarg, "none") || !strcmp(optarg, "clear"))
                    {
                      XFixesSetWindowShapeRegion(Dpy, win, kind,
                                                 0, 0, None);
                      break;
                    }

                  /* Go through the geometries specified in $optarg,
                   * set them in $rgrect and combine them together
                   * in the final $region. */
                  setop = UNION;
                  region = XFixesCreateRegion(Dpy, NULL, 0);
                  rgrect = XFixesCreateRegion(Dpy, NULL, 0);
                  for (;;)
                    {
                      Bool invert;

                      /* Take the inverse of the rectangle? */
                      if (*optarg == '~')
                        {
                          invert = True;
                          optarg++;
                        }
                      else
                        invert = False;

                      /* Set $rgrect. */
                      optarg = (char *)get_geometry(optarg, &rect);
                      XFixesSetRegion(Dpy, rgrect, &rect, 1);
                      if (invert)
                        { /* Invert $rgrect wrt to fullscreen. */
                          rect.x = rect.y = 0;
                          rect.width = DpyWidth;
                          rect.height = DpyHeight;
                          XFixesInvertRegion(Dpy, rgrect, &rect, rgrect);
                        }

                      /* Combine $region with $rgrect. */
                      if (setop == UNION)
                        XFixesUnionRegion(Dpy, region, region, rgrect);
                      else if (setop == INTERSECT)
                        XFixesIntersectRegion(Dpy, region, region, rgrect);
                      else if (setop == SUBTRACT)
                        XFixesSubtractRegion(Dpy, region, region, rgrect);

                      /* Determine the next combining operation. */
                      if (!*optarg)
                        break;
                      else if (*optarg == '|' || *optarg == ',')
                        setop = UNION;
                      else if (*optarg == '&')
                        setop = INTERSECT;
                      else if (*optarg == '/')
                        setop = SUBTRACT;
                      else
                        die("unknown set operation\n");
                      optarg++;
                    }

                  /* Set $win's shape and clean up. */
                  XFixesDestroyRegion(Dpy, rgrect);
                  XFixesSetWindowShapeRegion(Dpy, win, kind, 0, 0, region);
                  XFixesDestroyRegion(Dpy, region);
#else /* ! HAVE_XFIXES */
                  die("feature not available\n");
#endif
                }
              else
                { /* Resize and/or relocate the window. */
                  if (*get_geometry(optarg, &rect))
                    die("junk after geometry specification\n");
                  XMoveResizeWindow(Dpy, win,
                                   rect.x, rect.y,
                                   rect.width, rect.height);
                }
              break;
            }

          /* Window properties */
          case 'l': /* List */
            {
              int n, i;
              char *name;
              Atom *props;

              props = XListProperties(Dpy, win, &n);
              for (i = 0; i < n; i++)
                {
                  name = XGetAtomName(Dpy, props[i]);
                  if (Verbose)
                    fputs("  ", stdout);
                  puts(name);
                  XFree(name);
                }
              XFree(props);
              break;
            }
          case 'x': /* Delete property */
            XDeleteProperty(Dpy, win, get_atom(optarg));
            break;
          case 'i': /* Integer */
            process_iiwas(win, XA_INTEGER, optarg, propcmd);
            break;
          case 'I': /* Unsigned */
            process_iiwas(win, XA_CARDINAL, optarg, propcmd);
            break;
          case 'w': /* Window */
            process_iiwas(win, XA_WINDOW, optarg, propcmd);
            break;
          case 'a': /* Atom */
            process_iiwas(win, XA_ATOM, optarg, propcmd);
            break;
          case 's': /* String */
            process_iiwas(win, XA_STRING, optarg, propcmd);
            break;

          /* Change the focused window. */
          case 'f':
            {
              int revert;

              if (!strcmp(optarg, "parent"))
                revert = RevertToParent;
              else if (!strcmp(optarg, "root"))
                revert = RevertToPointerRoot;
              else if (!strcmp(optarg, "none"))
                revert = RevertToNone;
              else
                die("where to revert the focus if the window is gone?\n");
              XSetInputFocus(Dpy, win, revert, CurrentTime);
            }

          /* Send a ClientMessage */
          case 'C':
            {
              Window subject;
              char const *winspec;

              if ((winspec = isprefix(optarg, "win=")) != NULL)
                {
                  optarg = (char *)argv[optind++];
                  if (!(subject = choose_window(winspec)))
                    break;
                }
              else
                subject = win;

              send_client_message(win, subject, optarg);
              break;
            }

          /* Synthetize events */
          case 'E':
            {
              XEvent ev;
              char const *p;

              /* Get the event window. */
              memset(&ev, 0, sizeof(ev));
              if ((p = isprefix(optarg, "win=")) != NULL)
                {
                  optarg = (char *)argv[optind++];
                  if (!(ev.xany.window = choose_window(p)))
                    break;
                }
              else
                ev.xany.window = win;

              /* Handle abbreviations. */
              if (!strcmp(optarg, "obscured"))
                optarg = (char *)"visibility=obscured";
              else if (!strcmp(optarg, "unobscured"))
                optarg = (char *)"visibility=unobscured";

              /* Parse the event description. */
              if ((p = isprefix(optarg, "visibility=")) != NULL)
                {

                  if (!strcmp(p, "obscured"))
                    ev.xvisibility.state = VisibilityFullyObscured;
                  else if (!strcmp(p, "partial"))
                    ev.xvisibility.state = VisibilityPartiallyObscured;
                  else if (!strcmp(p, "unobscured"))
                    ev.xvisibility.state = VisibilityUnobscured;
                  else
                    die("unknown visibility\n");

                  ev.type = VisibilityNotify;
                  XSendEvent(Dpy, win, False, VisibilityChangeMask, &ev);
                }
              else if ((p = isprefix(optarg, "newprop=")) != NULL)
                {
                  ev.type = PropertyNotify;
                  ev.xproperty.atom = XInternAtom(Dpy, p, False);
                  ev.xproperty.state = PropertyNewValue;
                  XSendEvent(Dpy, win, False, PropertyChangeMask, &ev);
                }
              else if ((p = isprefix(optarg, "propdel=")) != NULL)
                {
                  ev.type = PropertyNotify;
                  ev.xproperty.atom = XInternAtom(Dpy, p, False);
                  ev.xproperty.state = PropertyDelete;
                  XSendEvent(Dpy, win, False, PropertyChangeMask, &ev);
                }
              else
                die("unknown event\n");
              break;
            }

#ifdef HAVE_FREMANTLE
          /* Control a hildon animation actor. */
          case 'A':
            {
              XEvent ev;
              unsigned n;

              ev.type = ClientMessage;
              ev.xclient.format = 32;
              ev.xclient.window = win;

              n = get_int_list(
                     ev.xclient.data.l, MEMBS_OF(ev.xclient.data.l),
                     get_key_val(&ev.xclient.message_type, optarg));

              if (ev.xclient.message_type == XInternAtom(Dpy,
                  "_HILDON_ANIMATION_CLIENT_MESSAGE_SHOW", False))
                {
                  /* If show and no opacity is given,
                   * make it fully opaque. */
                  if (n < 1)
                    ev.xclient.data.l[0] = 1;
                  if (ev.xclient.data.l[0] && n < 2)
                    ev.xclient.data.l[1] = 255;
                }
              else if (ev.xclient.message_type == XInternAtom(Dpy,
                  "_HILDON_ANIMATION_CLIENT_MESSAGE_ROTATION", False))
                /* Make the degree ClutterFixed. */
                ev.xclient.data.l[1] <<= 16;
              else if (ev.xclient.message_type == XInternAtom(Dpy,
                  "_HILDON_ANIMATION_CLIENT_MESSAGE_SCALE", False))
                {
                  unsigned one_hundredth = 0x28f;
                  unsigned long long accu;

                  /* Assume 1x1 scaling. */
                  if (n < 1)
                    ev.xclient.data.l[0] = 100;
                  if (n < 2)
                    ev.xclient.data.l[1] = 100;

                  /* Make the parameters ClutterFixed
                   * and divide them by 100. */
                  accu = ev.xclient.data.l[0];
                  accu <<= 16;
                  accu  *= one_hundredth;
                  accu >>= 16;
                  ev.xclient.data.l[0] = accu;

                  accu = ev.xclient.data.l[1];
                  accu <<= 16;
                  accu  *= one_hundredth;
                  accu >>= 16;
                  ev.xclient.data.l[1] = accu;
                }

              XSendEvent(Dpy, Root, False,
                         SubstructureNotifyMask, &ev);
              break;
            }
#endif /* HAVE_FREMANTLE */

          /* Change window attributes */
          case 'o':
            {
              int fs;
              long attrmask;
              XWMHints wmhints;
              char *opt, *name;
              XSetWindowAttributes attrs;
              struct { int state; Window icon; Bool isset; } state;

              fs = -1;
              name = NULL;
              attrmask = 0;
              memset(&state, 0, sizeof(state));
              memset(&wmhints, 0, sizeof(wmhints));
              while ((optarg = (char*)dup_optarg(optarg, &opt)) != NULL)
                {
                  Bool set;
                  char *dup;

                  dup = opt;
                  if (*opt == '!')
                    {
                      set = False;
                      opt++;
                    }
                  else
                    set = True;

                  if (!strcmp(opt, "name"))
                    { /* Unset the name. */
                      assert(!set);
                      XStoreName(Dpy, win, "");
                    }
                  else if ((name = (char*)isprefix(opt, "name=")) != NULL)
                    { /* For the idiots. */
                      assert(set);
                      XStoreName(Dpy, win, name);
                    }
                  else if (!strcmp(opt, "OR"))
                    {
                      attrs.override_redirect = set;
                      attrmask |= CWOverrideRedirect;
                    }
                  else if (!strcmp(opt, "focusable"))
                    {
                      wmhints.input = set;
                      wmhints.flags |= InputHint;
                    }
                  else if (!strcmp(opt, "starticonic"))
                    {
                      wmhints.initial_state = set
                        ? IconicState : NormalState;
                      wmhints.flags |= StateHint;
                    }
                  else if (!strcmp(opt, "normal"))
                    {
                      state.state = set ? NormalState : IconicState;
                      state.isset = True;
                    }
                  else if (!strcmp(opt, "iconic"))
                    {
                      state.state = set ? IconicState : NormalState;
                      state.isset = True;
                    }
                  else if (!strcmp(opt, "withdrawn"))
                    {
                      state.state = set ? WithdrawnState : NormalState;
                      state.isset = True;
                    }
                  else if (!strcmp(opt, "fs"))
                    fs =  set;
                  else if (!strcmp(opt, "nofs"))
                    fs = !set;
                  else
                    die("unknown flag\n");

                  free(dup);
                }

              if (attrmask)
                XChangeWindowAttributes(Dpy, win, attrmask, &attrs);
              if (wmhints.flags)
                  XSetWMHints(Dpy, win, &wmhints);
              if (state.isset >= 0)
                {
                  Atom wm_state;

                  wm_state = XInternAtom(Dpy, "WM_STATE", False);
                  XChangeProperty(Dpy, win, wm_state, wm_state,
                                  32, PropModeReplace,
                                  (unsigned char const *)&state, 2);
                }
              if (fs >= 0)
                { /* Fullscreen/unfullscreen/make it initially fs. */
                  char *iswm;
                  Atom wm_state;

                  wm_state = XInternAtom(Dpy, "WM_STATE", False);
                  iswm = get_property(win, wm_state, wm_state);
                  if (iswm)
                    {
                      XFree(iswm);
                      send_client_message(Root, win, fs
                        ? "_NET_WM_STATE=1,_NET_WM_STATE_FULLSCREEN"
                        : "_NET_WM_STATE=0,_NET_WM_STATE_FULLSCREEN");
                    }
                  else if (fs > 0)
                    {
                      Atom afs;

                      afs = XInternAtom(Dpy, "_NET_WM_STATE_FULLSCREEN",
                                        False);
                      set_properties(win,
                           XInternAtom(Dpy, "_NET_WM_STATE", False),
                           XA_ATOM, &afs, 1, PropModeReplace);
                    }
                  else
                    die("can't unfullscreen an unmanaged window\n");
                }
              break;
            }

          /* Mapping */
          case 'm':
            XMapWindow(Dpy, win);
            break;
          case 'u':
            XUnmapWindow(Dpy, win);
            break;

          /* Stacking */
          case 'R':
          case 'L':
            {
              Window whom;
              unsigned flags;
              XWindowChanges config;

              whom = win;
              flags = CWStackMode;
              config.sibling = None;
              if (win == Root)
                { /* $optarg is the win to stack. */
                  config.stack_mode = optchar == 'R' ? Above : Below;
                  if (!(whom = choose_window(optarg)))
                    break;
                }
              else if (!strcmp(optarg, "lo") || !strcmp(optarg, "bottom"))
                { /* $win -> bottom */
                  config.stack_mode = Below;
                }
              else if (!strcmp(optarg, "hi"))
                { /* Doesn't make sense with -L. */
                  assert(optchar == 'R');
                  config.stack_mode = Above;
                }
              else
                { /* $optarg is the desired sibling. */
                  config.stack_mode = optchar == 'R' ? Above : Below;
                  if (!(config.sibling  = choose_window(optarg)))
                    break;
                }

              if (config.sibling != None)
                flags |= CWSibling;
              XConfigureWindow(Dpy, whom, flags, &config);
              break;
            }

          /* Destruction */
          case 'd':
            delete_window(win);
            break;
          case 'D':
            XDestroyWindow(Dpy, win);
            break;
          case 'K':
            XKillClient(Dpy, win);
            break;

#ifdef HAVE_XTST
          /* Synthetise events */
          /* Fake a keypress. */
          case 'k':
            {
              unsigned delay;
              KeySym keysym;
              KeyCode keycode;
              char const *str, *modifiers;

              /*
               * optarg := [[<duration>]:][{ctrl|alt|fn}-]...
               *           {<keysym>|<string>}
               *
               * If we have a <duration> it has to be followed by a ':'.
               * Otherwise, ignore the possible leading ':'.
               */
              str = optarg;
              if (str[0] == ':')
                {
                  modifiers = ++str;
                  delay = 100;
                }
              else if (!(modifiers = get_duration(str, &delay, True))
                       || *modifiers++ != ':')
                {
                  modifiers = str;
                  delay = 100;
                }

              /* Check if we have $modifiers and press them. */
              str = send_modifiers(modifiers, True, CurrentTime);
              if ((keysym = XStringToKeysym(str)) != NoSymbol)
                { /* $str is a valid keysym */
                  keycode = XKeysymToKeycode(Dpy, keysym);
                  XTestFakeKeyEvent(Dpy, keycode, True, CurrentTime);
                  XTestFakeKeyEvent(Dpy, keycode, False, delay);
                }
              else
                { /* Send the characters of $str individually. */
                  char sym[2];

                  for (sym[1] = '\0'; (sym[0] = *str) != '\0'; str++)
                    {
                      if ((keysym = XStringToKeysym(sym)) == NoSymbol)
                        die("unknown keysym\n");
                      keycode = XKeysymToKeycode(Dpy, keysym);
                      XTestFakeKeyEvent(Dpy, keycode, True, CurrentTime);
                      XTestFakeKeyEvent(Dpy, keycode, False, delay);
                    }
                }
              if (modifiers != str)
                /* Release the modifiers. */
                send_modifiers(modifiers, False, delay);
              XSync(Dpy, False);
              break;
            }
#endif /* HAVE_XTST */

          /* Fake a click. */
          case 'c':
            {
              char const *p;
              XPoint old, mew;
              Bool tst, pressed;

              /* Abbreviations: pan/swipe? */
              if (!strcmp(optarg, "left"))
                p = "60x0.5br,100ms,0.5x0.5br";
              else if (!strcmp(optarg, "right"))
                p = "60x0.5bl,100ms,0.5x0.5bl";
              else if (!strcmp(optarg, "up"))
                p = "0.5x60bl,0.5x0.5bl";
              else if (!strcmp(optarg, "down"))
                p = "0.5x60tl,0.5x0.5tl";
              else if (!strcmp(optarg, "swleft")
                       || !strcmp(optarg, "swipe"))
                p = "5x0.5br,0.5x0.5br";
              else if (!strcmp(optarg, "swright"))
                p = "5x0.5bl,0.5x0.5bl";
              else if (!strcmp(optarg, "swup"))
                p = "0.5x5bl,0.5x0.6bl";
              else if (!strcmp(optarg, "swdown"))
                p = "0.5x5tl,0.5x0.6tl";
              else if (!strcmp(optarg, "swdown"))
                p = "0.5x5tl,0.5x0.6tl";
              else if (!strcmp(optarg, "qlb"))
                p = "1.0x0.5,300ms,160x0.5br@80ms";
              else
                p = optarg;

              /* Fake pointer events unless a window was specified.
               * (The window could be in the background.) */
              tst = win == Root;

              /*
               * The pattern to be parsed is:
               * 1. <xpos>[@<time>|!] or
               * 2. <xpos>[@<time>][[,<time>],<xpos>]*,<xpos>[@<time>|!]
               *
               * First move the cursor to the first <xpos>.  If it is
               * postfixed by '!' just press the button and we're done.
               * Otherwise if the expression consists of a single <xpos>
               * press and release the button in <time> (or 250ms), and
               * that's all.  Otherwise just press the button (optionally
               * waiting <time> between the move and the press), eat the
               * following ',', and move onto the next cycle.
               *
               * The subsequent cycles each parse [<time>,]<xpos> pairs.
               * Move the cursor to <xpos> in <time> (300ms by default)
               * smoothly, but with exponentially accelerating speed.
               *
               * If the expression's last <xpos> is postfixed with '!'
               * we finish with leaving the button pressed.  Otherwise
               * wait 250ms before releasing it or @<time> if it was
               * specified.
               */
              pressed = False;
              for (;;)
                {
                  char const *movetimestr;
                  unsigned movetime, clicktime;

                  /* Is it specified in how much time to move the
                   * cursor to the new <xpos>, given that the button
                   * is already pushed?  We need to look ahead a bit
                   * to disambiguate from a coordinate. */
                  if (pressed
                      && (movetimestr = get_duration(p, &movetime, True))
                          != NULL
                      && *movetimestr++ == ',')
                    p = movetimestr;
                  else
                    movetimestr = NULL;

                  /* Get the coordinate to move to, or where to begin. */
                  old = mew;
                  if (!(p = get_xpos(p, &mew)))
                    die("invalid coordinates\n");
                  if (mew.x < 0 || mew.y < 0)
                    die("negative coordinate\n");

                  /* How long to keep the button pressed? */
                  if (!pressed && !*p)
                    clicktime = 250;
                  else if (*p != '@')
                    clicktime = 0;
                  else if (!(p = get_duration(p+1, &clicktime, True)))
                    die("invalid time specification\n");

                  if (pressed)
                    { /* Move $old->$mew smoothly */
                      unsigned dist, slice, t;

                      /*
                       * Unless specified, calculate the time to move
                       * between the current and the new coordinates.
                       * Fro that, calculate $slice, the time between
                       * two movements.
                       */
                      dist  = (mew.x-old.x)*(mew.x-old.x);
                      dist += (mew.y-old.y)*(mew.y-old.y);
                      dist = sqrt(dist);
                      if (!movetimestr)
                        {
                          unsigned d_most, t_most;

                          /*
                           * Assume $d_most would be done in 950ms
                           * and do $dist in proportional time.
                           * (This was determined empirically to be
                           *  compatible with home panning.)
                           */
                          d_most = DpyWidth >= DpyHeight
                            ? DpyWidth : DpyHeight;
                          t_most = 950;
                          movetime = t_most * dist/d_most;
                          slice    = t_most/d_most;
                        }
                      else
                        slice = movetime/dist;

                      /* Since $slice determines our FPS limit to what
                       * we can reasonable expect from the display. */
                      if (slice < 15)
                        slice = 15;

                      /*
                       * Move with an exponentially accelerating pace.
                       * The easing function tells the ideal coordinates
                       * at any given time, but in practice we just take
                       * it at every $slice points.
                       */
                      t = 0;
                      do
                        {
                          XPoint p;

                          if (t + slice > movetime)
                            /* Do a shortened $slice and land at the
                             * exact destination coordinates. */
                            slice = movetime - t;
                          t += slice;

                          if (t < movetime)
                            {
                              float d;

                              /* Take the function at $t. */
                              d   = (pow(2, 10.0 * t/movetime) - 1)/(1024-1);
                              p.x = ((int)(mew.x-old.x))*d + old.x;
                              p.y = ((int)(mew.y-old.y))*d + old.y;
                              if (Verbose)
                                printf("move to %ux%u in %ums\n",
                                       p.x, p.y, slice);
                            }
                          else
                            { /* It's the last $slice */
                              p = mew;
                              if (Verbose)
                                printf("move to %ux%u in %ums "
                                       "(%ums in total)\n",
                                       p.x, p.y, slice, movetime);
                            }

                          pointer_event(win, MotionNotify, &p, slice, tst);
                        }
                      while (t < movetime);
                    }
                  else
                    { /* Not pressed yet, move to where it will be. */
                      if (Verbose)
                        printf("move to %ux%u\n", mew.x, mew.y);
                      pointer_event(win, MotionNotify, &mew, 0, tst);
                    }

                  /* End of instructions? */
                  if (*p == '!')
                    { /* Don't release. */
                      if (*++p != '\0')
                        die("junk after bang\n");
                      break;
                    }
                  else if (!*p)
                    { /* Release the button. */
                      if (!pressed)
                        { /* We got a single coordinate to click at. */
                          if (Verbose)
                            puts("press");
                          pointer_event(win, ButtonPress, &mew, 0, tst);
                        }
                      if (Verbose)
                        printf("release in %ums\n", clicktime);
                      pointer_event(win, ButtonRelease, &mew, clicktime, tst);
                      break;
                    }

                  /* We have another coordinate to move onto.  Time
                   * to press the button if this was the first one. */
                  if (!pressed)
                    {
                      pressed = True;
                      if (Verbose)
                        printf("press in %ums\n", clicktime);
                      pointer_event(win, ButtonPress, &mew, clicktime, tst);
                    }

                  if (*p++ != ',')
                    die("junk after coordinate specification\n");
                } /* until end of string */
              break;
            }

          /* Draw something on $win. */
          case 'G':
            {
              GC gc;
              XGCValues gcvals;
              unsigned long valmask;
              char const *cmd;

              gc = None;
              valmask = 0;
              if ((cmd = isprefix(optarg, "fill=")) != NULL)
                {   /* Draw a filled rectangle. */
                    XRectangle rect;

                    /* rect=<geo>[<color>] */
                    cmd = get_geometry(cmd, &rect);
                    if (strspn(cmd, "@%#"))
                      {
                        XWindowAttributes attrs;

                        get_win_attrs(win, &attrs, False, NULL);
                        cmd = get_color_pixel(attrs.colormap, cmd,
                                              &gcvals.foreground);
                        valmask |= GCForeground;
                      }

                    gc = XCreateGC(Dpy, win, valmask, &gcvals);
                    XFillRectangle(Dpy, win, gc,
                                   rect.x, rect.y, rect.width, rect.height);
                }
              else if ((cmd = isprefix(optarg, "text=")) != NULL)
                {   /* Draw text. */
                    /* text=<x>x<y>[@<color>],<msg>[,<font>] */
#ifndef HAVE_XFT /* Use good old XDrawText(). */
                    XPoint p;
                    XTextItem text;

                    /* Position */
                    if (!(cmd = get_xpos(cmd, &p)))
                      die("invalid coordinates\n");

                    /* Color */
                    if (strspn(cmd, "@%#"))
                      {
                        XWindowAttributes attrs;

                        get_win_attrs(win, &attrs, False, NULL);
                        cmd = get_color_pixel(attrs.colormap, cmd,
                                              &gcvals.background);
                        valmask |= GCBackground;
                      }

                    /* Text */
                    if (*cmd++ != ',')
                      die("where is the text?\n");
                    if (!(cmd = get_optarg(cmd,
                                           (char const **)&text.chars,
                                           (size_t *)&text.nchars)))
                      die("text expected\n");

                    /* Font */
                    if (*cmd)
                      {
                        char const *font;

                        if (!(cmd = get_optarg(cmd, &font, NULL)))
                          die("font expected\n");
                        if ((text.font = XLoadFont(Dpy, font)) == None)
                          die("font not found\n");
                      }
                    else
                      text.font = None;

                    /* Draw */
                    text.delta = 0;
                    gc = XCreateGC(Dpy, win, valmask, &gcvals);
                    XDrawText(Dpy, win, gc, p.x, p.y, &text, 1);
#else /* HAVE_XFT */
                    XPoint p;
                    XftDraw *xft;
                    XftFont *font;
                    XftColor color;
                    size_t ltext;
                    char const *text;
                    XWindowAttributes attrs;

                    get_win_attrs(win, &attrs, False, NULL);
                    assert(xft = XftDrawCreate(Dpy, win,
                                         attrs.visual, attrs.colormap));

                    /* Position */
                    if (!(cmd = get_xpos(cmd, &p)))
                      die("invalid coordinates\n");

                    /* Color */
                    if (strspn(cmd, "@%#"))
                      {
                        XColor xcolor;

                        cmd = get_xcolor(attrs.colormap, cmd, &xcolor);
                        color.pixel       = xcolor.pixel;
                        color.color.red   = xcolor.red;
                        color.color.green = xcolor.green;
                        color.color.blue  = xcolor.blue;
                        color.color.alpha = (color.pixel>>24) * 0x0101;
                      }
                    else
                      { /* Be black */
                        memset(&color, 0, sizeof(color));
                        color.color.alpha = 0xFFFF;
                      }

                    /* Text */
                    if (*cmd++ != ',')
                      die("where is the text?\n");
                    if (!(cmd = get_optarg(cmd, &text, &ltext)))
                      die("text expected\n");

                    /* Font */
                    if (*cmd)
                      {
                        char const *name;

                        if (!(cmd = get_optarg(cmd, &name, NULL)))
                          die("font expected\n");
                        if (!(font = XftFontOpenName(Dpy, Scr, name)))
                          die("font not found\n");
                      }
                    else
                        assert(font = XftFontOpenName(Dpy, Scr,
                                                      "default"));

                    /* Draw */
                    XftDrawString8(xft, &color, font, p.x, p.y,
                                   (FcChar8 const *)text, ltext);
                    XftFontClose(Dpy, font);
                    XftDrawDestroy(xft);
#endif /* HAVE_XFT */
                } /* if */
              else
                die("unknown primitive\n");

              /* Check that we could process all of $cmd. */
              if (*cmd != '\0')
                die("junk after graphic command\n");
              if (gc != None)
                XFreeGC(Dpy, gc);
              break;
            }

          /* Abbreviation */
          case 'X':
            {
              /* Hardwired $cmdline:s.  The zeroth element
               * has to be reserved for argv[0]. */
              static char const *hildon_tasw[] =
                { NULL, "-k", "ctrl-BackSpace", "root", NULL };
              static char const *hildon_rotate[] =
                { NULL, "-k", "ctrl-shift-fn-l", "root", NULL };
              static char const *hildon_portrait[] =
                { NULL, "-tI", "request=1", NULL };
              static char const *hildon_noncomp[] =
                { NULL, "-Ti", "noncomp=1", NULL };

              Window target;
              Bool assume_top;
              unsigned ncmds;
              char const *wmcmd, **cmdline;

              /* These are for -X *app* (app, mapp, umapp, ...). */
              char const *apwin, *mkapwin[10];

              /* Based on $optarg figure out $wmcmd or $cmdline. */
              ncmds = 1;
              cmdline = NULL;
              wmcmd = apwin = NULL;
              target = win;
              assume_top = False;
              if (!strcmp(optarg, "tasw"))
                { /* Go to tasw. */
                  Window wm;
                  char *wmname;
                  Bool is_hildon;

                  /* Try to make it animated on hildon-desktop. */
                  is_hildon = False;
                  if ((wm = find_wm_window()) != None
                      && (wmname = get_net_wm_name(wm)) != NULL)
                    {
                      is_hildon = !strcasecmp(wmname, "hildon-desktop");
                      XFree(wmname);
                    }

                  if (!is_hildon)
                    { /* Iconify the toplevel window. */
                      wmcmd = "WM_CHANGE_STATE=3";
                      target = find_topmost();
                    }
                  else
                    /* Press Ctrl-Backspace. */
                    cmdline = hildon_tasw;
                }
              else if (!strcmp(optarg, "iconify"))
                { /* Iconify the toplevel window. */
                  wmcmd = "WM_CHANGE_STATE=3";
                  assume_top = True;
                }
              else if (!strcmp(optarg, "fs")
                       || !strcmp(optarg, "fullscreen"))
                { /* Toggle fullscreen. */
                  wmcmd = "_NET_WM_STATE=2,_NET_WM_STATE_FULLSCREEN";
                  assume_top = True;
                }
              else if (!strcmp(optarg, "top"))
                { /* Present window. */
                  wmcmd = "_NET_ACTIVE_WINDOW";
                  if (implicit_win)
                    die("what to top?\n");
                }
              else if (!strcmp(optarg, "close"))
                { /* Close window. */
                  wmcmd = "_NET_CLOSE_WINDOW";
                  assume_top = True;
                }
              else if ((apwin = ismkapwin(optarg)) != NULL)
                { /* Create an application/desktop window. */
                  unsigned n;
                  Bool unmapped;

                  /* Leave unmapped? */
                  if ((unmapped = optarg[0] == 'u'))
                    optarg++;

                  /* Put $mkapwin together. */
                  n = 0;
                  mkapwin[n++] = NULL;
                  mkapwin[n++] = "-N";

                  if (strspn(apwin, "@%#"))
                    { /* Copy background color specification. */
                      char *tmp;

                      xmalloc(&tmp, 2+strlen(apwin)+1);
                      sprintf(tmp, "fs%s", apwin);
                      mkapwin[n++] = tmp;
                    }
                  else if (!*apwin)
                    mkapwin[n++] = "fs";
                  else
                    die("unknown abbreviation\n");

                  if (isprefix(optarg, "mapp"))
                    {
                      mkapwin[n++] = "-a";
                      mkapwin[n++] =
                        "_NET_WM_WINDOW_TYPE=_KDE_NET_WM_WINDOW_TYPE_OVERRIDE";
                      ncmds++;
                      mkapwin[n++] = "-p";
                      mkapwin[n++] = "append";
                      ncmds++;
                    }
                  mkapwin[n++] = "-a";
                  mkapwin[n++] = isprefix(optarg, "desktop")
                    ? "_NET_WM_WINDOW_TYPE=_NET_WM_WINDOW_TYPE_DESKTOP"
                    : "_NET_WM_WINDOW_TYPE=_NET_WM_WINDOW_TYPE_NORMAL";
                  ncmds++;

                  /* We don't actually support it, but claiming so
                   * prevents us from being slughtered by the wm. */
                  mkapwin[n++] = "-a";
                  mkapwin[n++] = "WM_PROTOCOLS=WM_DELETE_WINDOW";
                  ncmds++;

                  if (!unmapped)
                    {
                      mkapwin[n++] = "-m";
                      ncmds++;
                    }
                  mkapwin[n++] = NULL;
                  cmdline = mkapwin;
                  target = Root;
                }
              else if (!strcmp(optarg, "ping"))
                { /* Send a ping request, reply is not expected. */
                  static unsigned t;
                  XEvent ev;

                  /* Can't use send_client_message() because of $t. */
                  memset(&ev, 0, sizeof(ev));
                  ev.type = ClientMessage;
                  ev.xclient.format = 32;
                  ev.xclient.window = implicit_win ? find_topmost() : win;
                  ev.xclient.message_type = XInternAtom(Dpy,
                                               "WM_PROTOCOLS", False);
                  ev.xclient.data.l[0] = XInternAtom(Dpy,
                                               "_NET_WM_PING", False);
                  ev.xclient.data.l[1] = ++t;
                  ev.xclient.data.l[2] = ev.xclient.window;
                  XSendEvent(Dpy, ev.xclient.window, False,
                             NoEventMask, &ev);
                  break;

                }
              else if (!strcmp(optarg, "rotate"))
                { /* Force portrait/landscape. */
                  cmdline = hildon_rotate;
                }
              else if (!strcmp(optarg, "portrait"))
                { /* Toggle portrait. */
                  cmdline = hildon_portrait;
                  assume_top = True;
                }
              else if (!strcmp(optarg, "noncomp") || !strcmp(optarg, "nc"))
                { /* Toggle composition. */
                  cmdline = hildon_noncomp;
                  assume_top = True;
                }
              else
                die("unknown abbreviation\n");

              /* Based on $assume_top and $implicit_win, determine $target. */
              if (!implicit_win)
                assume_top = False;
              if (assume_top)
                target = find_topmost();

              /* Either have a simpler $wmcmd (-C) or a generic $cmdline. */
              if (cmdline)
                {
                  int saved_optind;
                  unsigned ncmdline;

                  /* Count $ncmdline. */
                  cmdline[0] = argv[0];
                  for (ncmdline = 0; cmdline[ncmdline]; ncmdline++)
                    /*puts(cmdline[ncmdline])*/;

                  /* Execute $cmdline. */
                  saved_optind = optind;
                  optind = 0;
                  target = command_block(ncmdline, cmdline, ncmds,
                                         target, implicit_win && !assume_top);
                  optind = saved_optind;

                  if (cmdline == mkapwin)
                    { /* We've created a new window. */
                      win = target;
                      implicit_win = False;

                      /* free() what we xmalloc()ed. */
                      if (*apwin)
                        free((char *)mkapwin[2]);
                    }
                }
              else
                {
                  assert(wmcmd);
                  send_client_message(Root, target, wmcmd);
                }
              break;
            }

          /* Execution control */
          case 'W':
            {
              unsigned ms;
              char const *p;

              /* All except -E <time> has been processed already. */
              if (!(p = get_duration(optarg, &ms, False)) || *p)
                die("invalid time specification\n");

              /* Flush the commands so we can wait for the effects. */
              XSync(Dpy, False);
              if (ms)
                { /* Wait $ms. */
                  if (Verbose && ms >= 1000)
                    {
                      fputs("Waiting...", stdout);
                      fflush(stdout);
                    }
                  usleep(ms*1000);
                  if (Verbose && ms >= 1000)
                    putchar('\n');
                }
              else
                { /* Wait until keypress. */
                  if (Is_interactive)
                    {
                      fputs("Enter", stdout);
                      fflush(stdout);
                    }
                  getchar();
                }
              break;
            }

          /* User tried to do use a feature which was not built in. */
          default:
            assert(optchar != EOF && optchar != 1);
            die("feature not available\n");
        } /* switch optchar */

      opt_Q = 0;
      propcmd = NONE;
    } /* for each command */

  return win;
} /* command_block */

/* The main function */
int main(int argc, char const *const *argv)
{
  int mark, cmdnext;
  int optchar, repeat, loop;
  Bool shall_quit, shall_retain;
  struct rlimit rlimit;

  /* Help? {{{ */
  if (!argv[1])
    {
      printf("%1$s -v\n"
              "%3$*2$s -[Q]q -[QQ]r -[QQQ]z <output>\n"
              "%3$*2$s -nN [!][OR=]<geo>[@{none|input|<color>}]\n"
              "%3$*2$s -g <geo> -g {shape|clip|input}={none|clear|[~]<geo>[{,|&|/}[~]<geo>]...}\n"
              "%3$*2$s -l -iIwas <key>\n"
              "%3$*2$s -x <key> -p prepend|append|toggle|flip\n"
              "%3$*2$s -s <key>=<val> -iIwa <key>={<integer>|<atom>},...\n"
              "%3$*2$s -f {parent|root|none}\n"
              "%3$*2$s -C [win=<event-window>] <msg>[=<param>,...]\n"
              "%3$*2$s -E obscured|unobscured|visibility={obscured|partial|unobscured}\n"
              "%3$*2$s -A show=<visible>[,<opacity>]\n"
              "%3$*2$s -A move=<x>,<y>[,<depth>]\n"
              "%3$*2$s -A anchor=<gravity>[,<x>,<y>]\n"
              "%3$*2$s -A rotate=<axis>,<degrees>[,<x>,<y>,<z>]\n"
              "%3$*2$s -A scale=<scale-x>[,<scale-y>]\n"
              "%3$*2$s -o name=NAME\n"
              "%3$*2$s -o {[!]{OR|focusable|starticonic|iconic|normal|withdrawn|fs}},...\n"
              "%3$*2$s -mu -R [<sibling>|hi|lo|bottom] -L [<sibling>|lo|bottom] -dDK\n"
              "%3$*2$s -k [[<duration>]:][{ctrl|alt|fn}-]...{<keysym>|<string>}\n"
              "%3$*2$s -c <x>x<y>[!]\n"
              "%3$*2$s -c <x1>x<y1>[[,<time>],<xi>x<yi>]*,<x2>x<y2>[!]\n"
              "%3$*2$s -c {[sw]{left|right|up|down}|swipe}\n"
              "%3$*2$s -G fill=<geo>[<color>]\n"
              "%3$*2$s -G text=<X>x<Y>[<color>],<text>[,<font>]\n"
              "%3$*2$s -X [u]{app|mapp|desktop}[#<alpha>][@none|<color>]\n"
              "%3$*2$s -X {top|iconify|close|tasw|fullscreen|fs|ping}\n"
              "%3$*2$s -X {portrait|rotate|noncomp|nc}\n"
              "%3$*2$s -W {quit|exit} -W {<time>|0}\n"
              "%3$*2$s -W loop[<number>times] -W <number>times\n"
              "%3$*2$s {<xid>|<name>|root|overlay|wm|new|top|top-<type>|select}...\n"
#ifdef CONFIG_FEATURES
# define FEATURE(str)      " " str
              "Built with" CONFIG_FEATURES ".\n"
#endif
              , argv[0], (unsigned)strlen(argv[0]), "");
      return 0;
    }
  /* }}} */

  /* Preprocess global options. {{{
   * They have to be pre-processed because they can be specified anywhere
   * in the command line. */
  shall_quit = shall_retain = False;
  while ((optchar = getopt(argc, (char **)argv, Optstring)) != EOF)
    {
      if (optchar == 'v')
        Verbose++;
      else if (optchar != 'W')
        continue;
      else if (!strcmp(optarg, "quit"))
        shall_quit = True;
      else if (!strcmp(optarg, "exit"))
        shall_quit = shall_retain = True;
    }
  /* preprocess }}} */

  /* Init {{{  */
  srand(time(NULL));
  Is_interactive = isatty(STDOUT_FILENO);

  /* Don't dump core on assert() failures. */
  memset(&rlimit, 0, sizeof(rlimit));
  setrlimit(RLIMIT_CORE, &rlimit);

  /* Connect to X. */
  if (!(Dpy = XOpenDisplay(getenv("DISPLAY"))))
      die("Couldn't open DISPLAY.\n");

  Scr = DefaultScreen(Dpy);
  Root = DefaultRootWindow(Dpy);
  DpyWidth = DisplayWidth(Dpy, Scr);
  DpyHeight = DisplayHeight(Dpy, Scr);
  Utf8 = XInternAtom(Dpy, "UTF8_STRING", False);
  /* }}} */

  /* Process command blocks until we're finished. {{{ */
  /*
   * mark         := where -W do will jump back
   * loop, repeat := the remaining number of loops or repeats to perform
   *                 (-1: spin forever)
   */
  Newborn = Root;
  mark = optind = 1;
  loop = repeat = 0;
  for (;;)
    {
      int cmdst;
      unsigned nwins, ncmds;
      char const *const *wins;
      Bool limbo, need_wins, seen_n, implicit;

      /*
       * Preprocess the command block to find out where it ends
       * and which windows should they operate on.
       *
       * cmdst := where the commands start for this command block
       * ncmds := the number of commands in this block
       * wins  := the windows to operate on
       * nwins := the number of windows in this block
       * limbo := whether we just encountered a two-argument command
       *          and we expect the other argument
       *
       * need_wins := whether there are commands in the block which we
       *              don't want to operate on the root window implicitly
       *              (like -mu)
       * seen_n := whether we've seen commands creating windows (like -Nn)
       *           which can be used by subsequent commands (so they don't
       *           $need_wins)
       */
      wins = NULL;
      cmdst = optind;
      ncmds = nwins = 0;
      limbo = need_wins = seen_n = False;
      for (;;)
        {
          /* cmdnext := the start of the next command block */
          cmdnext = optind;
          optchar = getopt(argc, (char **)argv, Optstring);
          if (optchar == EOF)
            break;
          if (optchar == '?')
            /* Unrecognized option, fail hard. */
            exit(1);
          if (optchar != 1)
            { /* This is a command. */
              if (limbo)
                die("required argument missing\n");
              limbo = False;

              if (wins)
                /* We've had window(s) so it's the beginning
                 * of the next cmdblock. */
                break;

              if (optchar == 'W')
                {
                  int n;

                  n = -1;
                  if (!strcmp(optarg, "do"))
                    {
                      if (ncmds)
                        break;
                      mark = optind;
                    }
                  else if (!strcmp(optarg, "repeat")
                           || sscanf(optarg, "repeat=%d", &n) == 1)
                    {
                      if (n < 0 || repeat++ < n)
                        { /* Repeat. */
                          cmdnext = mark;
                        }
                      else
                        { /* Don't repeat anymore, just end the cmdblock. */
                          cmdnext = optind;
                          repeat = 0;
                        }
                      ncmds++;
                      break;
                    }
                  else if (!strcmp(optarg, "loop")
                           || sscanf(optarg, "loop=%d", &n) == 1)
                    {
                      if (n < 0 || loop++ < n)
                        { /* Loop from the beginning. */
                          cmdnext = 1;
                        }
                      else
                        { /* Don't loop anymore. */
                          cmdnext = optind;
                          loop = 0;
                        }
                      ncmds++;
                      break;
                    }
                } /* optchar == 'W' */
              else if ((optchar == 'C' || optchar == 'E')
                       && !strncmp(optarg, "win=", 4))
                /* Optional additional argument given. */
                limbo = 1;

              switch (optchar)
                {
                  case 'v': case 'Q':
                  case 'R': case 'L':
                  case 'q': case 'r': case 'z':
                  case 'l': case 'x':
                  case 'i': case 'I': case 'w': case 'a': case 's':
                  case 'k': case 'c':
                  case 'C': case 'W':
                    /* For these commands we can assume the root window. */
                    break;
                  case 'n': case 'N':
                    if (optarg[0] != '!')
                      seen_n = True;
                    break;
                  case 'X':
                    if (ismkapwin(optarg))
                      seen_n = True;
                    break;
                  default:
                    if (!seen_n)
                      need_wins = True;
                    break;
                }

              ncmds++;
            } /* command */
          else if (!limbo)
            { /* This is a window. */
              nwins++;
              if (!wins)
                /* The first one! */
                wins = &argv[cmdnext];
            }
          else
            limbo = False;
        } /* preprocess cmdblock */

      if (!nwins)
        { /* This cmdblock didn't contain windows. */
          static char const *fake_root[] = { "root" };

          if (!ncmds)
            { /* Nor commands. */
              assert(optchar == EOF);
              break;
            }

          /* Would the commands accept an implicit root target window? */
          if (need_wins)
            die("must specify a window\n");

          implicit = True;
          wins = fake_root;
          nwins = MEMBS_OF(fake_root);
        }
      else
        { /* Windows without commands are OK. (Think map -v xxx.) */
          assert(wins != NULL);
          implicit = False;
        }

      /* Now, for each window, for each command. */
      for (; nwins > 0; nwins--, wins++)
        {
          Window win;

          /* Determine $win for the next set of commands. */
          if (!(win = choose_window(*wins)))
            /* "select" cancelled */
            continue;

          optind = cmdst;
          command_block(argc, argv, ncmds, win, implicit);
        } /* for each window */

        /* Continue from the next block. */
        optind = cmdnext;
    }
  /* main loop }}} */

  /* Finale {{{ */
  if (shall_retain)
    { /* Tell X to retain the windows we created after quitting. */
      XSetCloseDownMode(Dpy, RetainPermanent);
    }
  else if (!shall_quit && NWindows > 0)
    {
      /* We've created a window.  Wait for key press otherwise
       * it would disappear instantly. */
      XSync(Dpy, False);
      if (Is_interactive)
        {
          fputs("Enter", stdout);
          fflush(stdout);
        }
      if (!Is_interactive || getchar() == EOF)
        pause();
    }
  XCloseDisplay(Dpy);
  /* }}} */

  return 0;
} /* main */

#define END_OF_PROGRAM          /* End of map.c */
END_OF_PROGRAM
/* vim: set cindent  cinoptions=>4,n-2,{2,^-2,\:2,=2,g0,h2,p5,t0,+2,(0,u0,w1,m1: */
/* vim: set expandtab shiftwidth=2 softtabstop=2: */
/* vim: set formatoptions=croql foldmethod=marker: */
