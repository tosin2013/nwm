#include <errno.h>
#include <locale.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>

#include "nwm.h"
#include "x11_misc.cc"


// INTERNAL API

// the nwm struct can be static, since it's not like you can
// run multiple instances of a window manager anyway

static void nwm_scan_windows();
static void nwm_add_window(Window win, XWindowAttributes *wa);
static void nwm_update_window(Window win);
static void nwm_remove_window(Window win, Bool destroyed);

static void nwm_scan_monitors();
void nwm_add_monitor();
void nwm_remove_monitor();
void nwm_update_selected_monitor();

static void nwm_emit(callback_map event, nwm_event *ev);

// these should go into a function dispach table indexed by the Xevent type
static void event_buttonpress(XEvent *e);
static void event_clientmessage(XEvent *e);
static void event_configurerequest(XEvent *e);
static void event_configurenotify(XEvent *e);
static void event_destroynotify(XEvent *e);
static void event_enternotify(XEvent *e);
static void event_focusin(XEvent *e);
static void event_keypress(XEvent *e);
static void event_maprequest(XEvent *e);
static void event_propertynotify(XEvent *e);
static void event_unmapnotify(XEvent *e);

void GrabMouseRelease(Window id);


// then we need another set piece, which binds Node to the nwm library

// just like X11, we should have a single event type which is
// a union of the different kinds of events.

static const char broken[] = "broken";

static void (*handler[LASTEvent]) (XEvent *) = {
  [ButtonPress] = event_buttonpress,
  [ClientMessage] = event_clientmessage,
  [ConfigureRequest] = event_configurerequest,
  [ConfigureNotify] = event_configurenotify,
  [DestroyNotify] = event_destroynotify,
  [EnterNotify] = event_enternotify,
  [FocusIn] = event_focusin,
  [KeyPress] = event_keypress,
  [MapRequest] = event_maprequest,
  [PropertyNotify] = event_propertynotify,
  [UnmapNotify] = event_unmapnotify
};

int nwm_init() {
  int screen;
  XSetWindowAttributes wa;

  nwm.total_monitors = 0;
  nwm.keys = NULL;
  nwm.numlockmask = 0;

  // open the display
  if ( ( nwm.dpy = XOpenDisplay(NULL) ) == NULL ) {
    fprintf( stdout, "cannot connect to X server %s\n", XDisplayName(NULL));
    exit( -1 );
  }
  // set error handler
  XSetErrorHandler(xerror);
  XSync(nwm.dpy, False);

  // take the default screen
  screen = DefaultScreen(nwm.dpy);
  // get the root window and screen geometry
  nwm.root = RootWindow(nwm.dpy, screen);
  nwm.screen_width = DisplayWidth(nwm.dpy, screen);
  nwm.screen_height = DisplayHeight(nwm.dpy, screen);
  // update monitor geometry (and create nwm.monitor)
  nwm_scan_monitors();

  // subscribe to root window events e.g. SubstructureRedirectMask
  wa.event_mask = SubstructureRedirectMask|SubstructureNotifyMask|ButtonPressMask
                  |EnterWindowMask|LeaveWindowMask|StructureNotifyMask
                  |PropertyChangeMask;
  XSelectInput(nwm.dpy, nwm.root, wa.event_mask);
  nwm_grab_keys(nwm.dpy, nwm.root);

  nwm_scan_windows();

  // emit a rearrange
  nwm_emit(onRearrange, NULL);
  XSync(nwm.dpy, False);
  // return the connection number so the node binding can use it with libev.
  return XConnectionNumber(nwm.dpy);
}

static void nwm_scan_windows() {
  unsigned int i, num;
  Window d1, d2, *wins = NULL;
  XWindowAttributes watt;
  // XQueryTree() function returns the root ID, the parent window ID, a pointer to
  // the list of children windows (NULL when there are no children), and
  // the number of children in the list for the specified window.
  if(XQueryTree(nwm.dpy, nwm.root, &d1, &d2, &wins, &num)) {
    for(i = 0; i < num; i++) {
      // if we can't read the window attributes,
      // or the window is a popup (transient or override_redirect), skip it
      if(!XGetWindowAttributes(nwm.dpy, wins[i], &watt)
      || watt.override_redirect || XGetTransientForHint(nwm.dpy, wins[i], &d1)) {
        continue;
      }
      // visible or minimized window ("Iconic state")
      if(watt.map_state == IsViewable )//|| getstate(wins[i]) == IconicState)
        nwm_add_window(wins[i], &watt);
    }
    for(i = 0; i < num; i++) { /* now the transients */
      if(!XGetWindowAttributes(nwm.dpy, wins[i], &watt))
        continue;
      if(XGetTransientForHint(nwm.dpy, wins[i], &d1)
      && (watt.map_state == IsViewable )) //|| getstate(wins[i]) == IconicState))
        nwm_add_window(wins[i], &watt);
    }
    if(wins) {
      XFree(wins);
    }
  }
}

// replace this with a linked list implementation
// The linked list should provide:
// add(item)
// some(callback) --> for writing a find function
// remove(index)


void nwm_empty_keys() {
  Key* curr = nwm.keys;
  Key* next;
  while(curr != NULL) {
    next = curr->next;
    free(curr);
    curr = next;
  }
}

void nwm_add_key(Key** keys, KeySym keysym, unsigned int mod) {
  Key* curr;
  if(!(curr = (Key*)calloc(1, sizeof(Key)))) {
    fprintf( stdout, "fatal: could not malloc() %lu bytes\n", sizeof(Key));
    exit( -1 );
  }
  curr->keysym = keysym;
  curr->mod = mod;
  curr->next = *keys;
  *keys = curr;
}

void nwm_grab_keys(Display* dpy, Window root) {
  nwm.numlockmask = updatenumlockmask(dpy);
  { // update numlockmask first!
    unsigned int i;
    unsigned int modifiers[] = { 0, LockMask, nwm.numlockmask, nwm.numlockmask|LockMask };
    XUngrabKey(nwm.dpy, AnyKey, AnyModifier, nwm.root);
    for(Key* curr = nwm.keys; curr != NULL; curr = curr->next) {
      fprintf( stdout, "grab key -- key: %li modifier %d \n", curr->keysym, curr->mod);
      // also grab the combinations of screen lock and num lock (as those should not matter)
      for(i = 0; i < 4; i++) {
        XGrabKey(nwm.dpy, XKeysymToKeycode(nwm.dpy, curr->keysym), curr->mod | modifiers[i], nwm.root, True, GrabModeAsync, GrabModeAsync);
      }
    }
  }
}

void nwm_emit_function() {
  // This function should store the emit function in nwm.
  // That function then gets called from nwm_emit
}

void nwm_emit(callback_map event, nwm_event *ev) {
  // NOP until I figure out how to call Node from here...
}

void nwm_loop() {
  XEvent event;

  // main event loop
  while(XPending(nwm.dpy)) {
    XNextEvent(nwm.dpy, &event);
    fprintf(stdout, "got event %s (%d).\n", event_names[event.type], event.type);

    if(handler[event.type]) {
      handler[event.type](&event); /* call handler */
    } else {
      fprintf(stdout, "Did nothing with %s (%d)\n", event_names[event.type], event.type);
    }
  }
}

void nwm_move_window(Window win, int x, int y) {
  fprintf( stdout, "MoveWindow: id=%li x=%d y=%d \n", win, x, y);
  XMoveWindow(nwm.dpy, win, x, y);
  XFlush(nwm.dpy);
}

void nwm_resize_window(Window win, int width, int height) {
  fprintf( stdout, "ResizeWindow: id=%li width=%d height=%d \n", win, width, height);
  XResizeWindow(nwm.dpy, win, width, height);
  XFlush(nwm.dpy);
}

void nwm_focus_window(Window win){
  fprintf( stdout, "FocusWindow: id=%li\n", win);
  grabButtons(win, True);
  XSetInputFocus(nwm.dpy, win, RevertToPointerRoot, CurrentTime);
  Atom atom = XInternAtom(nwm.dpy, "WM_TAKE_FOCUS", False);
  SendEvent(nwm.dpy, win, atom);
  XFlush(nwm.dpy);
  nwm.selected = win;
}

void nwm_kill_window(Window win) {
  XEvent ev;
  // check whether the client supports "graceful" termination
  if(isprotodel(nwm.dpy, win)) {
    ev.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = XInternAtom(nwm.dpy, "WM_PROTOCOLS", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = XInternAtom(nwm.dpy, "WM_DELETE_WINDOW", False);
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(nwm.dpy, win, False, NoEventMask, &ev);
  } else {
    XGrabServer(nwm.dpy);
    XSetErrorHandler(xerrordummy);
    XSetCloseDownMode(nwm.dpy, DestroyAll);
    XKillClient(nwm.dpy, win);
    XSync(nwm.dpy, False);
    XSetErrorHandler(xerror);
    XUngrabServer(nwm.dpy);
  }
}

void nwm_configure_window(Window win, int x, int y, int width, int height,
  int border_width, int above, int detail, int value_mask) {
  XWindowChanges wc;
  wc.x = x;
  wc.y = y;
  wc.width = width;
  wc.height = height;
  wc.border_width = border_width;
  wc.sibling = above;
  wc.stack_mode = detail;
  XConfigureWindow(nwm.dpy, win, value_mask, &wc);
}

void nwm_notify_window(Window win, int x, int y, int width, int height,
    int border_width, int above, int detail, int value_mask) {
  XConfigureEvent ce;

  ce.type = ConfigureNotify;
  ce.display = nwm.dpy;
  ce.event = win;
  ce.window = win;
  ce.x = x;
  ce.y = y;
  ce.width = width;
  ce.height = height;
  ce.border_width = border_width;
  ce.above = None;
  ce.override_redirect = False;
  XSendEvent(nwm.dpy, win, False, StructureNotifyMask, (XEvent *)&ce);
}

void nwm_add_window(Window win, XWindowAttributes *wa) {
  Window trans = None;
  Bool isfloating = False;
  XConfigureEvent ce;
  nwm_event event_data;

  // check whether the window is transient
  XGetTransientForHint(nwm.dpy, win, &trans);
  isfloating = (trans != None);

  fprintf( stderr, "Create client %li (x %d, y %d, w %d, h %d, float %d)\n", win, wa->x, wa->y, wa->width, wa->height, isfloating);
  // emit onAddWindow in Node.js
  event_data.type = nwm_Window;
  event_data.window.id = win;
  event_data.window.x = wa->x;
  event_data.window.y = wa->y;
  event_data.window.height = wa->height;
  event_data.window.width = wa->width;
  event_data.window.isfloating = isfloating;
  nwm_emit(onAddWindow, event_data);

  // push the window id so we know what windows we've seen
// TODO TODO TODO    this->seen_windows.push_back(win);

  nwm_update_window(win); // update title and class, emit onUpdateWindow

  // configure the window
  ce.type = ConfigureNotify;
  ce.display = nwm.dpy;
  ce.event = win;
  ce.window = win;
  ce.x = wa->x;
  ce.y = wa->y;
  ce.width = wa->width;
  ce.height = wa->height;
  ce.border_width = wa->border_width;
  ce.above = None;
  ce.override_redirect = False;

  fprintf( stdout, "manage: x=%d y=%d width=%d height=%d \n", ce.x, ce.y, ce.width, ce.height);

  XSendEvent(nwm.dpy, win, False, StructureNotifyMask, (XEvent *)&ce);
  // subscribe to window events
  XSelectInput(nwm.dpy, win, EnterWindowMask|FocusChangeMask|PropertyChangeMask|StructureNotifyMask);
  grabButtons(win, False);

  if(isfloating) {
    XRaiseWindow(nwm.dpy, win);
  }

  // move and (finally) map the window
  XMoveResizeWindow(nwm.dpy, win, ce.x, ce.y, ce.width, ce.height);
  XMapWindow(nwm.dpy, win);
}

void nwm_update_window(Window win) {
  char name[256];
  char klass[256];
  char instance[256];
  // update title
  Atom NetWMName = XInternAtom(nwm.dpy, "_NET_WM_NAME", False);
  if(!gettextprop(nwm.dpy, win, NetWMName, name, sizeof name))
    gettextprop(nwm.dpy, win, XA_WM_NAME, name, sizeof name);
  if(name[0] == '\0') /* hack to mark broken clients */
    strcpy(name, broken);
  // update class
  XClassHint ch = { 0 };
  if(XGetClassHint(nwm.dpy, win, &ch)) {
    if(ch.res_class) {
      strncpy(klass, ch.res_class, 256-1 );
    } else {
      strncpy(klass, broken, 256-1 );
    }
    klass[256-1] = 0;
    if(ch.res_name) {
      strncpy(instance, ch.res_name, 256-1 );
    } else {
      strncpy(instance, broken, 256-1 );
    }
    instance[256-1] = 0;
    if(ch.res_class)
      XFree(ch.res_class);
    if(ch.res_name)
      XFree(ch.res_name);
  }

  nwm_window_title event_data;

  event_data.type = nwm_Windowtitle;
  event_data.id = win;
  event_data.title = name;
  event_data.instance = instance;
  event_data.klass = klass;

  // emit onUpdateWindow
  nwm_emit(onUpdateWindow, event_data);
}

void nwm_remove_window(Window win, Bool destroyed) {
  fprintf( stdout, "HandleRemove - emit onRemovewindow, %li\n", win);
  nwm_window event_data;
  event_data.type = nwm_Window;
  event_data.id = win;

  // emit a remove
  nwm_emit(onRemoveWindow, event_data);
  if(!destroyed) {
    XGrabServer(nwm.dpy);
    XUngrabButton(nwm.dpy, AnyButton, AnyModifier, win);
    XSync(nwm.dpy, False);
    XUngrabServer(nwm.dpy);
  }
  // remove from seen list of windows
  // TODO TODO TODO std::vector<Window>& vec = nwm.seen_windows; // use shorter name
  // TODO TODO TODO std::vector<Window>::iterator newEnd = std::remove(vec.begin(), vec.end(), win);
  // TODO TODO TODO vec.erase(newEnd, vec.end());

  fprintf( stdout, "Focusing to root window\n");
  nwm_focus_window(nwm.root);
  fprintf( stdout, "Emitting rearrange\n");
  nwm_emit(onRearrange, NULL);
}


static void nwm_scan_monitors() {
  int i, nn;
  unsigned int j;
  XineramaScreenInfo *info = NULL;
  XineramaScreenInfo *unique = NULL;
  // no Xinerama
  if(!XineramaIsActive(nwm.dpy) && nwm.total_monitors == 0) {
    nwm.total_monitors++;
    // emit ADD MONITOR
    nwm_monitor event_data;

    event_data.type = nwm_Monitor;
    event_data.id = 0;
    event_data.x = 0;
    event_data.y = 0;
    event_data.width = nwm.screen_width;
    event_data.height = nwm.screen_height;

    nwm_emit(onAddMonitor, event_data);
    nwm_update_selected_monitor();
    return;
  }

  // with Xinerama
  fprintf( stdout, "Xinerama active\n");
  info = XineramaQueryScreens(nwm.dpy, &nn);

  fprintf( stdout, "Monitors known %d, monitors found %d\n", nwm.total_monitors, nn);
  /* only consider unique geometries as separate screens */
  if(!(unique = (XineramaScreenInfo *)malloc(sizeof(XineramaScreenInfo) * nn))) {
    fprintf( stdout, "fatal: could not malloc() %lu bytes\n", sizeof(XineramaScreenInfo) * nn);
    exit( -1 );
  }
  for(i = 0, j = 0; i < nn; i++)
    if(isuniquegeom(unique, j, &info[i]))
      memcpy(&unique[j++], &info[i], sizeof(XineramaScreenInfo));
  XFree(info);
  nn = j;
  if(nwm.total_monitors <= nn) {
    // update monitor dimensions
    //  We just emit the monitors and don't track the dimensions in the binding at all.
    for(i = 0; i < nn; i++) {
      nwm_monitor event_data;

      event_data.type = nwm_Monitor;
      event_data.id = i;
      event_data.x = unique[i].x_org;
      event_data.y = unique[i].y_org;
      event_data.width = unique[i].width;
      event_data.height = unique[i].height;

      fprintf( stdout, "Emit monitor %d\n", i);
      if(i >= nwm.total_monitors) {
        nwm_emit(onAddMonitor, event_data);
        nwm.total_monitors++;
      } else {
        nwm_emit(onUpdateMonitor, event_data);
      }
    }
  } else { // fewer monitors available nn < n
    fprintf( stdout, "Fewer monitors available %d %d\n", nwm.total_monitors, nn);
    for(i = nn; i < nwm.total_monitors; i++) {
      // emit REMOVE MONITOR (i)
      nwm_monitor event_data;

      event_data.type = nwm_Monitor;
      event_data.id = i;
      nwm_emit(onRemoveMonitor, event_data);
      // remove monitor
      nwm.total_monitors--;
    }
  }
  free(unique);
  nwm_update_selected_monitor();
}


// update the selected monitor on Node.js side
// NOTE: We can probably get rid of this altogether, since it isn't essential.
// Node will keep the focused monitor as the first one, but that should be OK.
void nwm_update_selected_monitor() {
  int x, y;
  if(getrootptr(nwm.dpy, nwm.root, &x, &y)) {
    fprintf(stdout, "EmitEnterNotify wid = %li \n", nwm.root);
    nwm_monitor event_data;

    event_data.type = nwm_EnterNotify;
    event_data.id = nwm.root;
    event_data.x = x;
    event_data.y = y;

    nwm_emit(onEnterNotify, event_data);
  }
}

static void event_buttonpress(XEvent *e) {
  fprintf(stdout, "Handle(mouse)ButtonPress\n");
  nwm_emit(onMouseDown, e);
  GrabMouseRelease(e->xbutton.window);
}

void GrabMouseRelease(Window id) {
  // disabled for now
  return;
/*
  XEvent ev;
  int x, y;
  Local<Value> argv[1];

  if(XGrabPointer(nwm.dpy, nwm.root, False,
    ButtonPressMask|ButtonReleaseMask|PointerMotionMask, GrabModeAsync,
    GrabModeAsync, None, XCreateFontCursor(nwm.dpy, XC_fleur), CurrentTime) != GrabSuccess) {
    return;
  }
  if(!nwm.getrootptr(&x, &y)) {
    return;
  }
  do{
    XMaskEvent(nwm.dpy, ButtonPressMask|ButtonReleaseMask|PointerMotionMask|ExposureMask|SubstructureRedirectMask, &ev);
    switch(ev.type) {
      case ConfigureRequest:
        // handle normally
        break;
      case Expose:
        // handle normally
        break;
      case MapRequest:
        // handle normally
        break;
      case MotionNotify:
        {
          argv[0] = NodeWM::makeMouseDrag(id, x, y, ev.xmotion.x, ev.xmotion.y); // , ev.state);
          nwm.Emit(onMouseDrag, 1, argv);
        }
        break;
    }
  } while(ev.type != ButtonRelease);

  XUngrabPointer(nwm.dpy, CurrentTime); */
}

static void event_clientmessage(XEvent *e) {
  XClientMessageEvent *cme = &e->xclient;
  Atom NetWMState = XInternAtom(nwm.dpy, "_NET_WM_STATE", False);
  Atom NetWMFullscreen = XInternAtom(nwm.dpy, "_NET_WM_STATE_FULLSCREEN", False);
  nwm_window_fullscreen event_data;

  if(cme->message_type == NetWMState && cme->data.l[1] == NetWMFullscreen) {
    event_data.type = nwm_Window;
    event_data.id = cme->window;
    if(cme->data.l[0]) {
      XChangeProperty(nwm.dpy, cme->window, NetWMState, XA_ATOM, 32,
                      PropModeReplace, (unsigned char*)&NetWMFullscreen, 1);
      XRaiseWindow(nwm.dpy, cme->window);
      event_data.fullscreen = 1;
    }
    else {
      XChangeProperty(nwm.dpy, cme->window, NetWMState, XA_ATOM, 32,
                      PropModeReplace, (unsigned char*)0, 0);
      event_data.fullscreen = 0;
    }
    nwm_emit(onFullscreen, event_data);
  }
}

static void event_configurerequest(XEvent *e) {
  // dwm checks for whether the window is known,
  // only unknown windows are allowed to configure themselves.
  // Node should call AllowReconfigure()or ConfigureNotify() + Move/Resize etc.
  nwm_emit(onConfigureRequest, e);
}

static void event_configurenotify(XEvent *e) {
  XConfigureEvent *ev = &e->xconfigure;

  if(ev->window == nwm.root) {
    nwm.screen_width = ev->width;
    nwm.screen_height = ev->height;
    // update monitor structures
    nwm_scan_monitors();
    nwm_emit(onRearrange, NULL);
  }
}

static void event_destroynotify(XEvent *e) {
  nwm_remove_window(e->xdestroywindow.window, True);
}

static void event_enternotify(XEvent *e) {
  fprintf(stdout, "HandleEnterNotify wid = %li \n", e->xcrossing.window);
  nwm_emit(onEnterNotify, e);
}

static void event_focusin(XEvent *e) {
  XFocusChangeEvent *ev = &e->xfocus;
  fprintf(stdout, "HandleFocusIn for window id %li\n", ev->window);
  if(nwm.selected && ev->window != nwm.selected){
    Bool found = True;
    // TODO TODO TODO  FIXME (std::find(nwm.seen_windows.begin(), nwm.seen_windows.end(), ev->window) != nwm.seen_windows.end());
    // Preventing focus stealing
    // http://mail.gnome.org/archives/wm-spec-list/2003-May/msg00013.html
    // We will always revert the focus to whatever was last set by Node (e.g. enterNotify).
    // This prevents naughty applications from stealing the focus permanently.
    if(found) {
      // only revert if the change was to a top-level window that we manage
      // For instance, FF menus would otherwise get reverted..
      fprintf(stdout, "Reverting focus change by window id %li to %li \n", ev->window, nwm.selected);
      nwm_focus_window(nwm.selected);
    }
  }
}

static void event_keypress(XEvent *e) {
  KeySym keysym;
  XKeyEvent *ev;

  ev = &e->xkey;
  keysym = XKeycodeToKeysym(nwm.dpy, (KeyCode)ev->keycode, 0);

  nwm_keypress event_data;
  // we always unset numlock and LockMask since those should not matter
  event_data.type = nwm_Keypress;
  event_data.x = ev->x;
  event_data.y = ev->y;
  event_data.keycode = ev->keycode;
  event_data.keysym = keysym;
  event_data.modifier = (ev->state & ~(nwm.numlockmask|LockMask));

  // call the callback in Node.js, passing the window object...
  nwm_emit(onKeyPress, event_data);
}

static void event_maprequest(XEvent *e) {
  // read the window attrs, then add it to the managed windows...
  XWindowAttributes wa;
  XMapRequestEvent *ev = &e->xmaprequest;
  if(!XGetWindowAttributes(nwm.dpy, ev->window, &wa)) {
    fprintf(stdout, "XGetWindowAttributes failed\n");
    return;
  }
  if(wa.override_redirect)
    return;
  fprintf(stdout, "MapRequest\n");
  Bool found = True;
  // TODO TODO TODO FIXME (std::find(nwm.seen_windows.begin(), nwm.seen_windows.end(), ev->window) != nwm.seen_windows.end());
  if(!found) {
    // only map new windows
    nwm_add_window(ev->window, &wa);
    // emit a rearrange
    nwm_emit(onRearrange, NULL);
  } else {
    fprintf(stdout, "Window is known\n");
  }
}

static void event_propertynotify(XEvent *e) {
  XPropertyEvent *ev = &e->xproperty;
  // could be used for tracking hints, transient status and window name
  if((ev->window == nwm.root) && (ev->atom == XA_WM_NAME)) {
    // the root window name has changed
  } else if(ev->state == PropertyDelete) {
    return; // ignore property deletes
  } else {
    Atom NetWMName = XInternAtom(nwm.dpy, "_NET_WM_NAME", False);
    if(ev->atom == XA_WM_NAME || ev->atom == NetWMName) {
      nwm_update_window(ev->window); // update title and class
    }
  }
}

static void event_unmapnotify(XEvent *e) {
  nwm_remove_window(e->xunmap.window, False);
}