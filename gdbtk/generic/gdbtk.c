/* Startup code for Insight
   Copyright (C) 1994-2019 Free Software Foundation, Inc.

   Written by Stu Grossman <grossman@cygnus.com> of Cygnus Support.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.  */

#include "defs.h"
#undef STRINGIFY		// Avoid conflict.

#include "inferior.h"
#include "symfile.h"
#include "objfiles.h"
#include "gdbcore.h"
#include "tracepoint.h"
#include "demangle.h"
#include "common/version.h"
#include "top.h"
#include "annotate.h"
#include "exceptions.h"
#include "event-loop.h"
#include "main.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* tcl header files includes varargs.h unless HAS_STDARG is defined,
   but gdb uses stdarg.h, so make sure HAS_STDARG is defined.  */
#define HAS_STDARG 1

#include <tcl.h>
#include <tk.h>

/* Satisfy new constant declaration macros for old Tcl versions. */

#ifndef CONST84
#define CONST84
#endif
#ifndef CONST86
#define CONST86
#endif

#include "guitcl.h"
#include "gdbtk.h"
#include "gdbtk-interp.h"

#include <fcntl.h>
#include <sys/stat.h>
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#include <sys/time.h>
#include <signal.h>

#include <string.h>
#include "dis-asm.h"
#include "gdbcmd.h"
#include "gdb_select.h"

#ifdef __CYGWIN32__
#include <sys/cygwin.h>		/* for cygwin32_attach_handle_to_fd */
#endif

extern void _initialize_gdbtk (void);

#ifndef __MINGW32__
/* For unix natives, we use a timer to periodically keep the gui alive.
   See comments before x_event. */
static sigset_t nullsigmask;
static struct sigaction act1, act2;
static struct itimerval it_on, it_off;

static void
x_event_wrapper (int signo)
{
  x_event (signo);
}
#endif

/* This variable holds the main process id. */

long gdbtk_pid = -1;

/*
 * This variable controls the interaction with an external editor.
 */

char *external_editor_command = NULL;

extern
#ifdef __cplusplus
	"C"
#endif
	int Tktable_Init (Tcl_Interp *interp);

void gdbtk_init (void);

void gdbtk_interactive (void);

static void tk_command (const char *, int);

static int target_should_use_timer (struct target_ops *t);

int target_is_native (struct target_ops *t);

int gdbtk_test (char *);

static void view_command (const char *, int);

static int gdbtk_timer_going = 0;

/* linked variable used to tell tcl what the current thread is */
int gdb_context = 0;

/* This variable is true when the inferior is running.  See note in
 * gdbtk.h for details.
 */
int running_now;

/* This variable holds the name of a Tcl file which should be sourced by the
   interpreter when it goes idle at startup. Used with the testsuite. */
static std::string gdbtk_source_filename;

bool gdbtk_disable_write = true;

#ifndef _WIN32

/* Supply malloc calls for tcl/tk.  We do not want to do this on
   Windows, because Tcl_Alloc is probably in a DLL which will not call
   the mmalloc routines.
   We also don't need to do it for Tcl/Tk8.1, since we locally changed the
   allocator to use malloc & free. */

#if TCL_MAJOR_VERSION == 8 && TCL_MINOR_VERSION == 0
char *
TclpAlloc (unsigned int size)
{
  return xmalloc (size);
}

char *
TclpRealloc (char *ptr, unsigned int size)
{
  return xrealloc (ptr, size);
}

void
TclpFree (char *ptr)
{
  free (ptr);
}
#endif /* TCL_VERSION == 8.0 */

#endif /* ! _WIN32 */

#ifdef _WIN32

/* On Windows, if we hold a file open, other programs can't write to
 * it.  In particular, we don't want to hold the executable open,
 * because it will mean that people have to get out of the debugging
 * session in order to remake their program.  So we close it, although
 * this will cost us if and when we need to reopen it.
 */

void
close_bfds (void)
{
  struct objfile *o;

  ALL_OBJFILES (o)
    {
      if (o->obfd != NULL)
	bfd_cache_close (o->obfd);
    }

  if (exec_bfd != NULL)
    bfd_cache_close (exec_bfd);
}

#endif /* _WIN32 */


/* TclDebug (const char *fmt, ...) works just like printf() but
 * sends the output to the GDB TK debug window.
 * Not for normal use; just a convenient tool for debugging
 */

void
TclDebug (char level, const char *fmt,...)
{
  va_list args;
  char *buf;
  const char *v[3];
  char *merge;
  const char *priority;
  Tcl_Interp *interp = gdbtk_get_interp ()->tcl;

  switch (level)
    {
    case 'W':
      priority = "W";
      break;
    case 'E':
      priority = "E";
      break;
    case 'X':
      priority = "X";
      break;
    default:
      priority = "I";
    }

  va_start (args, fmt);


  buf = xstrvprintf (fmt, args);
  va_end (args);

  v[0] = "dbug";
  v[1] = priority;
  v[2] = buf;

  merge = Tcl_Merge (3, v);
  if (Tcl_Eval (interp, merge) != TCL_OK)
    Tcl_BackgroundError (interp);
  Tcl_Free (merge);
  free(buf);
}

/* Tcl notifier using gdb event loop as external event loop. */
/* Multithreading not supported: both event loops must run in the
   same thread. */

/* File handler Tcl event. */
typedef struct
  {
    Tcl_Event header;                   /* Standard tcl event info. */
    int fd;                             /* File descriptor. */
  }
gdbtk_notifier_file_handler_event;

/* Per file handler data. */
typedef struct _gdbtk_notifier_file_data gdbtk_notifier_file_data;
struct _gdbtk_notifier_file_data
  {
    gdbtk_notifier_file_data *next;     /* Next notifier file data. */
    int fd;                             /* File descriptor. */
    int mask;                           /* Tcl event mask. */
    int readymask;                      /* Pending mask. */
    Tcl_FileProc *proc;                 /* Tcl callback procedure. */
    ClientData clientData;              /* Tcl client data. */
  };

/* Notifier data. */
#define GDBTK_NOTIFIER_IN_TCL           (1 << 0) /* Currently executing tcl. */
#define GDBTK_NOTIFIER_REDISPATCH	(1 << 1) /* Request tcl redispatch. */

typedef struct
  {
    gdbtk_notifier_file_data *filelist; /* List of gdbtk_notifier_file_data. */
    struct async_event_handler *schedule; /* Gdb event to tcl event loop. */
    int service_mode;                   /* Current service mode. */
    unsigned int flags;                 /* Various flags. */
  }
gdbtk_notifier_state;

static Tcl_ThreadDataKey        thrdatakey;


static gdbtk_notifier_state *
notifier_state (void)
{
  gdbtk_notifier_state *notif;

  notif = (gdbtk_notifier_state *) Tcl_GetThreadData (&thrdatakey,
                                                      sizeof *notif);
  return notif;
}

/* Defer tcl action if tcl is already executing. */
static int
gdbtk_notifier_defer (gdbtk_notifier_state *notif)
{
  if (!(notif->flags & GDBTK_NOTIFIER_IN_TCL))
    return 0;
  notif->flags |= GDBTK_NOTIFIER_REDISPATCH;
  return 1;
}

/* Request execution of Tcl_DoOneEvent. */
static void
gdbtk_notifier_reschedule_tcl (void)
{
  gdbtk_notifier_state *notif = notifier_state ();

  notif->flags &= ~GDBTK_NOTIFIER_REDISPATCH;
  if (!gdbtk_notifier_defer (notif))
    mark_async_event_handler (notif->schedule);         /* Activate. */
}

/* Callback from gdb event loop to process a Tcl event. */
static void
gdbtk_notifier_schedule_proc (gdb_client_data clientData)
{
  gdbtk_notifier_state *notif = (gdbtk_notifier_state *) clientData;

  /* Avoid recursively calling Tcl_DoOneEvent. */
  if (!gdbtk_notifier_defer (notif))
    {
      notif->flags |= GDBTK_NOTIFIER_IN_TCL;
      if (Tcl_DoOneEvent (TCL_DONT_WAIT) > 0)
        notif->flags |= GDBTK_NOTIFIER_REDISPATCH;      /* Might be more. */
      notif->flags &= ~GDBTK_NOTIFIER_IN_TCL;

      /* If Tcl activation has been requested since entered, reactivate
         immediately. */
      if (notif->flags & GDBTK_NOTIFIER_REDISPATCH)
        gdbtk_notifier_reschedule_tcl ();
    }
}

/* Search a file handler data structure by its file descriptor. */
static gdbtk_notifier_file_data **
gdbtk_notifier_get_file_data (int fd)
{
  gdbtk_notifier_state *notif = notifier_state ();
  gdbtk_notifier_file_data **dataptr = &notif->filelist;
  gdbtk_notifier_file_data *data;

  for (; (data = *dataptr); dataptr = &data->next)
    if (data->fd == fd)
      break;
  return dataptr;
}

/* File handler Tcl event comes here. */
static int
gdbtk_notifier_file_handler_event_proc (Tcl_Event *evptr, int flags)
{
  gdbtk_notifier_file_handler_event *feptr =
    (gdbtk_notifier_file_handler_event *) evptr;
  gdbtk_notifier_file_data *data;
  int mask;

  if (!(flags & TCL_FILE_EVENTS))
    return 0;   /* File event processing not requested. */

  data = *gdbtk_notifier_get_file_data (feptr->fd);

  if (data)
    {
      mask = data->mask & data->readymask;      /* Wanted events only. */
      data->readymask = 0;                      /* Allow subsequent event. */
      if (mask)
        data->proc (data->clientData, mask);    /* Tcl file event handler. */
    }

  return 1;     /* Event processed. */
}

/* File handler gdb event comes here. */
static void
gdbtk_notifier_file_proc (int error, gdb_client_data clientData)
{
  gdbtk_notifier_file_data *data = (gdbtk_notifier_file_data *) clientData;
  int tclmask = 0;
  struct timeval timeout;
  fd_set readset;
  fd_set writeset;
  fd_set exceptset;

  /* gdb does not pass the event types to this callback, thus we must reselect
     to get them. */

  FD_ZERO (&readset);   FD_SET (data->fd, &readset);
  FD_ZERO (&writeset);  FD_SET (data->fd, &writeset);
  FD_ZERO (&exceptset); FD_SET (data->fd, &exceptset);
  timeout.tv_sec = 0;
  timeout.tv_usec = 0;
  if (select (data->fd + 1, &readset, &writeset, &exceptset, &timeout) < 0)
    tclmask = TCL_READABLE;
  else
    {
      if (FD_ISSET (data->fd, &readset))
        tclmask |= TCL_READABLE;
      if (FD_ISSET (data->fd, &writeset))
        tclmask |= TCL_WRITABLE;
      if (FD_ISSET (data->fd, &exceptset))
        tclmask |= TCL_EXCEPTION;
    }

  if (tclmask)
    {
      if (!data->readymask)     /* Do not queue an event if another pending. */
        {
          /* Queue a Tcl event for that file. */
          gdbtk_notifier_file_handler_event *feptr =
            (gdbtk_notifier_file_handler_event *) ckalloc (sizeof *feptr);

          feptr->header.proc = gdbtk_notifier_file_handler_event_proc;
          feptr->fd = data->fd;
          Tcl_QueueEvent ((Tcl_Event *) feptr, TCL_QUEUE_TAIL);
          gdbtk_notifier_reschedule_tcl ();
        }
      data->readymask = tclmask;
    }
}

/* Tcl notifier procedure to start an event servicing timer.
 * Not needed in our context: tcl event rescheduling is explicitly handled. */
static void
gdbtk_notifier_set_timer (CONST86 Tcl_Time *timeptr)
{
  (void) timeptr;
}

/* Tcl notifier procedure to wait for an event.
 * Use gdb event loop wait function. */
static int
gdbtk_notifier_wait_for_event (CONST86 Tcl_Time *timeptr)
{
  int msec = -1;

  if (timeptr)
    msec = timeptr->sec * 1000 + (timeptr->usec + 500) / 1000;
  return gdb_do_one_event (msec);
}

/* Tcl notifier procedure to delete a file handler.
   Remove it from the gdb queue. */
static void
gdbtk_notifier_delete_file_handler (int fd)
{
  gdbtk_notifier_file_data **dataptr = gdbtk_notifier_get_file_data (fd);
  gdbtk_notifier_file_data *data = *dataptr;

  delete_file_handler (fd);
  if (data)
    {
      /* Release associated data. */
      *dataptr = data->next;
      xfree (data);
    }
}

/* Tcl notifier procedure to create a new file handler.
   Propagate call to gdb file handler. */
static void
gdbtk_notifier_create_file_handler (int fd, int tclmask, Tcl_FileProc *proc,
                                    ClientData clientData)
{
  gdbtk_notifier_state *notif = notifier_state ();
  gdbtk_notifier_file_data *data;
  int gdbmask = 0;

  data = *gdbtk_notifier_get_file_data (fd);
  if (data)
    gdbtk_notifier_delete_file_handler (fd);

  /* Convert Tcl notation mask to gdb notation. */
  if (tclmask & TCL_READABLE)
    gdbmask |= GDB_READABLE;
  if (tclmask & TCL_WRITABLE)
    gdbmask |= GDB_WRITABLE;
  if (tclmask & TCL_EXCEPTION)
    gdbmask |= GDB_EXCEPTION;

  /* Allocate and populate our private data structure, then submit to gdb. */
  data = (gdbtk_notifier_file_data *) xcalloc (1, sizeof *data);
  data->fd = fd;
  data->mask = tclmask;
  data->proc = proc;
  data->clientData = clientData;
  data->next = notif->filelist;
  notif->filelist = data;
  add_file_handler (fd, gdbtk_notifier_file_proc, (gdb_client_data) data,
		    gdbmask);
}

/* Tcl notifier procedure to initialize the notifier. */
static ClientData
gdbtk_notifier_initialize (void)
{
  gdbtk_notifier_state *notif = notifier_state ();

  /* Create the gdb event propagating gdb event loop to Tcl. */
  notif->schedule =
                create_async_event_handler (gdbtk_notifier_schedule_proc,
                (gdb_client_data) notif);
  Tcl_SetServiceMode (TCL_SERVICE_ALL); /* Needs event servicing. */
  return (ClientData) notif;
}

/* Tcl notifier procedure to terminate the notifier. */
static void
gdbtk_notifier_finalize (ClientData clientData)
{
  gdbtk_notifier_state *notif = (gdbtk_notifier_state *) clientData;

  gdbtk_notifier_set_timer (NULL);      /* Cancel timer, if some. */
  if (notif->schedule)          /* Release the reschedule gdb event. */
    delete_async_event_handler (&notif->schedule);
  /* Release all file handlers. */
  while (notif->filelist)
    gdbtk_notifier_delete_file_handler (notif->filelist->fd);
  notif->flags = 0;
}

/* Tcl notifier procedure to interrupt the event waiting.
   Since we are not supporting multithreading, this should never be needed.
   However if called, Tcl activation is rescheduled. */
static void
gdbtk_notifier_alert (ClientData clientData)
{
  (void) clientData;
  gdbtk_notifier_reschedule_tcl ();
}

/* Tcl notifier hook procedure to capture the requested service mode. */
static void
gdbtk_notifier_service_mode_hook (int mode)
{
  gdbtk_notifier_state *notif = notifier_state ();

  notif->service_mode = mode;
}

/* Notifier definition structure. */
static Tcl_NotifierProcs gdbtk_notifier_procs =
  {
    (Tcl_SetTimerProc *) gdbtk_notifier_set_timer,
    (Tcl_WaitForEventProc *) gdbtk_notifier_wait_for_event,
    (Tcl_CreateFileHandlerProc *) gdbtk_notifier_create_file_handler,
    (Tcl_DeleteFileHandlerProc *) gdbtk_notifier_delete_file_handler,
    (Tcl_InitNotifierProc *) gdbtk_notifier_initialize,
    (Tcl_FinalizeNotifierProc *) gdbtk_notifier_finalize,
    (Tcl_AlertNotifierProc *) gdbtk_notifier_alert,
    (Tcl_ServiceModeHookProc *) gdbtk_notifier_service_mode_hook
};


/* Install the local notifier. */
static void
gdbtk_install_notifier (void)
{
  Tcl_SetNotifier (&gdbtk_notifier_procs);
}

/* Uninistall the local notifier. */
void
gdbtk_uninstall_notifier (void)
{
  Tcl_NotifierProcs noprocs;

  memset ((char *) &noprocs, 0, sizeof noprocs);
  Tcl_SetNotifier (&noprocs);
}


/*
 * The rest of this file contains the start-up, and event handling code for gdbtk.
 */

/* Come here during long calculations to check for GUI events.  Usually invoked
   via the QUIT macro.  */

void
gdbtk_interactive (void)
{
  /* Tk_DoOneEvent (TK_DONT_WAIT|TK_IDLE_EVENTS); */
}

/* Start a timer which will keep the GUI alive while in target_wait. */
void
gdbtk_start_timer (void)
{
  static int first = 1;

  if (first)
    {
      /* first time called, set up all the structs */
      first = 0;
#ifndef __MINGW32__
      sigemptyset (&nullsigmask);

      act1.sa_handler = x_event_wrapper;
      act1.sa_mask = nullsigmask;
      act1.sa_flags = 0;

      act2.sa_handler = SIG_IGN;
      act2.sa_mask = nullsigmask;
      act2.sa_flags = 0;

      it_on.it_interval.tv_sec = 0;
      it_on.it_interval.tv_usec = 250000;	/* .25 sec */
      it_on.it_value.tv_sec = 0;
      it_on.it_value.tv_usec = 250000;

      it_off.it_interval.tv_sec = 0;
      it_off.it_interval.tv_usec = 0;
      it_off.it_value.tv_sec = 0;
      it_off.it_value.tv_usec = 0;
#endif
    }

  if (target_should_use_timer (current_top_target ()))
    {
      if (!gdbtk_timer_going)
	{
#ifndef __MINGW32__
	  sigaction (SIGALRM, &act1, NULL);
	  setitimer (ITIMER_REAL, &it_on, NULL);
#endif
	  gdbtk_timer_going = 1;
	}
    }
  return;
}

/* Stop the timer if it is running. */
void
gdbtk_stop_timer (void)
{
  if (gdbtk_timer_going)
    {
      gdbtk_timer_going = 0;
#ifndef __MINGW32__
      setitimer (ITIMER_REAL, &it_off, NULL);
      sigaction (SIGALRM, &act2, NULL);
#endif
    }
  return;
}

/* Should this target use the timer? See comments before
   x_event for the logic behind all this. */
static int
target_should_use_timer (struct target_ops *t)
{
  return target_is_native (t);
}

/* Is T a native target? */
int
target_is_native (struct target_ops *t)
{
  const char *name = t->shortname ();

  if (strcmp (name, "exec") == 0 || strcmp (name, "hpux-threads") == 0
      || strcmp (name, "child") == 0 || strcmp (name, "procfs") == 0
      || strcmp (name, "solaris-threads") == 0
      || strcmp (name, "linuxthreads") == 0
      || strcmp (name, "multi-thread") == 0
      || strcmp (name, "native") == 0)
    return 1;

  return 0;
}

/* gdbtk_init installs this function as a final cleanup.  */

static void
gdbtk_cleanup (PTR dummy)
{
  Tcl_Eval (gdbtk_get_interp ()->tcl, "gdbtk_cleanup");
  Tcl_Finalize ();
}


/* Initialize gdbtk.  This involves creating a Tcl interpreter,
 * defining all the Tcl commands that the GUI will use, pointing
 * all the gdb "hooks" to the correct functions,
 * and setting the Tcl auto loading environment so that we can find all
 * the Tcl based library files.
 */

void
gdbtk_init (gdbtk_interp *interp)
{
  int element_count;
  const char **exec_path;
  CONST char *internal_exec_name;
  Tcl_Obj *command_obj;
  int running_from_builddir;

  /* First init tcl and tk. */
  gdbtk_install_notifier ();
  Tcl_FindExecutable (get_gdb_program_name ());
  interp->tcl = Tcl_CreateInterp ();

  if (!interp->tcl)
    error ("Tcl_CreateInterp failed");

#ifdef TCL_MEM_DEBUG
  Tcl_InitMemory (interp->tcl);
#endif

  /* Set up some globals used by gdb to pass info to gdbtk
     for start up options and the like */
  Tcl_SetVar2 (interp->tcl, "GDBStartup", "inhibit_prefs",
               string_printf ("%d", inhibit_gdbinit).c_str (), TCL_GLOBAL_ONLY);

  /* Note: Tcl_SetVar2() treats the value as read-only (making a
     copy).  Unfortunately it does not mark the parameter as
     ``const''. */
  Tcl_SetVar2 (interp->tcl, "GDBStartup",
               "host_name", (char*) host_name, TCL_GLOBAL_ONLY);
  Tcl_SetVar2 (interp->tcl, "GDBStartup",
               "target_name", (char*) target_name, TCL_GLOBAL_ONLY);
  {
#ifdef __CYGWIN
    char *srcdir = (char *) alloca (cygwin_posix_to_win32_path_list_buf_size (SRC_DIR));
    cygwin_posix_to_win32_path_list (SRC_DIR, srcdir);
#else /* !__CYGWIN */
    const char *srcdir = SRC_DIR;
#endif /* !__CYGWIN */
    Tcl_SetVar2 (interp->tcl, "GDBStartup", "srcdir", srcdir, TCL_GLOBAL_ONLY);
  }

  /* This is really lame, but necessary. We need to set the path to our
     library sources in the global GDBTK_LIBRARY. This was only necessary
     for running from the build dir, but when using a system-supplied
     Tcl/Tk/Itcl, we cannot rely on the user installing Insight into
     the same tcl library directory. */

  internal_exec_name = Tcl_GetNameOfExecutable ();

  Tcl_SplitPath ((char *) internal_exec_name, &element_count, &exec_path);
  if (strcmp (exec_path[element_count - 2], "bin") == 0)
    running_from_builddir = 0;
  else
    running_from_builddir = 1;
  Tcl_Free ((char *) exec_path);

  /* This seems really complicated, and that's because it is.
     We would like to preserve the following ways of running
     Insight (and having it work, of course):

     1. Installed using installed Tcl et al
     2. From build directory using installed Tcl et al
     3. Installed using Tcl et al from the build tree
     4. From build directory using Tcl et al from the build tree

     When running from the builddir (nos. 2,4), we set all the
     *_LIBRARY variables manually to point at the proper locations in
     the source tree. (When Tcl et al are installed, their
     corresponding variables get set incorrectly, but tcl_findLibrary
     will still find the correct installed versions.)

     When not running from the build directory, we must set GDBTK_LIBRARY,
     just in case we are running from a non-standard install directory
     (i.e., Tcl and Insight were installed into two different
     install directories). One snafu: we use libgui's Paths
     environment variable to do this, so we cannot actually
     set GDBTK_LIBRARY until libgui is initialized. */

  if (running_from_builddir)
    {
      /* We check to see if TCL_LIBRARY, TK_LIBRARY,
	 ITCL_LIBRARY, ITK_LIBRARY, and maybe a couple other
	 environment variables have been set (we don't want
	 to override the User's settings).

	 If the *_LIBRARY variable is is not set, point it at
	 the source directory. */
      static char set_lib_paths_script[] = "\
          set srcDir [file dirname $GDBStartup(srcdir)]\n\
          if {![info exists env(TCL_LIBRARY)]} {\n\
              set env(TCL_LIBRARY) [file join $srcDir tcl library]\n\
          }\n\
\
          if {![info exists env(TK_LIBRARY)]} {\n\
              set env(TK_LIBRARY) [file join $srcDir tk library]\n\
          }\n\
\
          if {![info exists env(ITCL_LIBRARY)]} {\n\
              set env(ITCL_LIBRARY) [file join $srcDir itcl itcl library]\n\
          }\n\
\
          if {![info exists env(ITK_LIBRARY)]} {\n\
              set env(ITK_LIBRARY) [file join $srcDir itcl itk library]\n\
          }\n\
\
          if {![info exists env(IWIDGETS_LIBRARY)]} {\n\
              set env(IWIDGETS_LIBRARY) \
                     [file join $srcDir itcl iwidgets generic]\n\
          }\n\
\
	  if {![info exists env(GDBTK_LIBRARY)]} {\n\
	      set env(GDBTK_LIBRARY) [file join $GDBStartup(srcdir) gdbtk library]\n\
	  }\n\
\
          # Append the directory with the itcl/itk/iwidgets pkg indexes\n\
          set startDir [file dirname [file dirname [info nameofexecutable]]]\n\
          lappend ::auto_path [file join $startDir itcl itcl]\n\
          lappend ::auto_path [file join $startDir itcl itk]\n\
          lappend ::auto_path [file join $startDir itcl iwidgets]\n";

      command_obj = Tcl_NewStringObj (set_lib_paths_script, -1);
      Tcl_IncrRefCount (command_obj);
      Tcl_EvalObj (interp->tcl, command_obj);
      Tcl_DecrRefCount (command_obj);
    }

  make_final_cleanup (gdbtk_cleanup, NULL);

  if (Tcl_Init (interp->tcl) != TCL_OK)
    error ("Tcl_Init failed: %s", Tcl_GetStringResult (interp->tcl));

  /* Initialize the Paths variable.  */
  if (ide_initialize_paths (interp->tcl, "") != TCL_OK)
    error ("ide_initialize_paths failed: %s",
           Tcl_GetStringResult (interp->tcl));

  if (Tk_Init (interp->tcl) != TCL_OK)
    error ("Tk_Init failed: %s", Tcl_GetStringResult (interp->tcl));

  if (Tktable_Init (interp->tcl) != TCL_OK)
    error ("Tktable_Init failed: %s", Tcl_GetStringResult (interp->tcl));

  Tcl_StaticPackage (interp->tcl, "Tktable", Tktable_Init,
		     (Tcl_PackageInitProc *) NULL);

  /* If we are not running from the build directory,
     initialize GDBTK_LIBRARY. See comments above. */
  if (!running_from_builddir)
    {
      static char set_gdbtk_library_script[] = "\
	  if {![info exists env(GDBTK_LIBRARY)]} {\n\
	      set env(GDBTK_LIBRARY) [file join [file dirname [file dirname $Paths(guidir)]] insight1.0]\n\
	  }\n";

      command_obj = Tcl_NewStringObj (set_gdbtk_library_script, -1);
      Tcl_IncrRefCount (command_obj);
      Tcl_EvalObj (interp->tcl, command_obj);
      Tcl_DecrRefCount (command_obj);
    }

  /* Get the main process id. */
  gdbtk_pid = gdbtk_getpid();

  /*
   * These are the commands to do some Windows Specific stuff...
   */

#ifdef TK_PLATFORM_WIN
  if (ide_create_messagebox_command (interp->tcl) != TCL_OK)
    error ("messagebox command initialization failed");
  /* On Windows, create a sizebox widget command */
#if 0
  if (ide_create_sizebox_command (interp->tcl) != TCL_OK)
    error ("sizebox creation failed");
#endif
  if (ide_create_winprint_command (interp->tcl) != TCL_OK)
    error ("windows print code initialization failed");
  if (ide_create_win_grab_command (interp->tcl) != TCL_OK)
    error ("grab support command initialization failed");
  if (ide_create_shell_execute_command (interp->tcl) != TCL_OK)
    error ("cygwin shell execute command initialization failed");
#endif
#ifdef __CYGWIN32__
  /* Path conversion functions.  */
  if (ide_create_cygwin_path_command (interp->tcl) != TCL_OK)
    error ("cygwin path command initialization failed");
#endif

  /* Only for testing -- and only when it can't be done any
     other way. */
  if (cyg_create_warp_pointer_command (interp->tcl) != TCL_OK)
    error ("warp_pointer command initialization failed");

  /*
   * This adds all the Gdbtk commands.
   */

  if (Gdbtk_Init (interp->tcl) != TCL_OK)
    {
      error ("Gdbtk_Init failed: %s", Tcl_GetStringResult (interp->tcl));
    }

  Tcl_StaticPackage (interp->tcl, "Insight", Gdbtk_Init, NULL);

  /* Add a back door to Tk from the gdb console... */

  add_com ("tk", class_obscure, tk_command,
	   "Send a command directly into tk.");

  add_com ("view", class_obscure, view_command,
	   "View a location in the source window.");

  /*
   * Set the variable for external editor:
   */

  if (external_editor_command != NULL)
    {
      Tcl_SetVar (interp->tcl, "external_editor_command",
		  external_editor_command, 0);
      xfree (external_editor_command);
      external_editor_command = NULL;
    }

#ifdef __CYGWIN32__
  (void) FreeConsole ();
#endif
}

void
gdbtk_source_start_file (gdbtk_interp *interp)
{
  /* find the gdb tcl library and source main.tcl */
#ifdef NO_TCLPRO_DEBUGGER
  static char script[] = "\
proc gdbtk_find_main {} {\n\
    global Paths GDBTK_LIBRARY\n\
    rename gdbtk_find_main {}\n\
    tcl_findLibrary insight 1.0 {} main.tcl GDBTK_LIBRARY GDBTKLIBRARY\n\
    set Paths(appdir) $GDBTK_LIBRARY\n\
}\n\
gdbtk_find_main";
#else
    static char script[] = "\
proc gdbtk_find_main {} {\n\
    global Paths GDBTK_LIBRARY env\n\
    rename gdbtk_find_main {}\n\
    if {[info exists env(DEBUG_STUB)]} {\n\
        source $env(DEBUG_STUB)\n\
        debugger_init\n\
        set debug_startup 1\n\
    } else {\n\
        set debug_startup 0\n\
    }\n\
    tcl_findLibrary insight 1.0 {} main.tcl GDBTK_LIBRARY GDBTK_LIBRARY\n\
    set Paths(appdir) $GDBTK_LIBRARY\n\
}\n\
gdbtk_find_main";
#endif /* NO_TCLPRO_DEBUGGER */

  /* now enable gdbtk to parse the output from gdb */
  gdbtk_disable_write = false;

  if (Tcl_GlobalEval (interp->tcl, (char *) script) != TCL_OK)
    {
      const char *msg;

      /* Force errorInfo to be set up propertly.  */
      Tcl_AddErrorInfo (interp->tcl, "");
      msg = Tcl_GetVar (interp->tcl, "errorInfo", TCL_GLOBAL_ONLY);

#ifdef _WIN32
      /* On windows, display the error using a pop-up message box.
	 If GDB wasn't started from the DOS prompt, the user won't
	 get to see the failure reason.  */
      MessageBox (NULL, msg, NULL, MB_OK | MB_ICONERROR | MB_TASKMODAL);
#else
      /* gdb_stdout is already pointing to OUR stdout, so we cannot
	 use *_[un]filtered here. Since we're "throwing" an exception
         which should cause us to exit, just print out the error
         to stderr. */
      fputs (msg, stderr);
#endif

      throw_error (GENERIC_ERROR, "%s", msg);
    }

  /* Now source in the filename provided by the --tclcommand option.
     This is mostly used for the gdbtk testsuite... */

  if (gdbtk_source_filename.length ())
    {
      Tcl_Eval (interp->tcl,
                ("after idle source " + gdbtk_source_filename).c_str ());
      gdbtk_source_filename.clear ();
    }
}

/* gdbtk_test is used in main.c to validate the -tclcommand option to
   gdb, which sources in a file of tcl code after idle during the
   startup procedure. */

int
gdbtk_test (char *filename)
{
  if (access (filename, R_OK) != 0)
    return 0;
  gdbtk_source_filename = filename;
  return 1;
}

/* Come here during initialize_all_files () */

void
_initialize_gdbtk (void)
{
#ifdef __CYGWIN__
  /* Current_interpreter not set yet, so we must check
     if "interpreter_p" is set to "insight" to know if
     insight is GOING to run. */
  if (strcmp (interpreter_p, "insight") != 0)
    {
      DWORD ft = GetFileType (GetStdHandle (STD_INPUT_HANDLE));

      switch (ft)
	{
	case FILE_TYPE_DISK:
	case FILE_TYPE_CHAR:
	case FILE_TYPE_PIPE:
	  break;
	default:
	  AllocConsole ();
	  cygwin_attach_handle_to_fd ("/dev/conin", 0,
				      GetStdHandle (STD_INPUT_HANDLE),
				      1, GENERIC_READ);
	  cygwin_attach_handle_to_fd ("/dev/conout", 1,
				      GetStdHandle (STD_OUTPUT_HANDLE),
				      0, GENERIC_WRITE);
	  cygwin_attach_handle_to_fd ("/dev/conout", 2,
				      GetStdHandle (STD_ERROR_HANDLE),
				      0, GENERIC_WRITE);
	  break;
	}
    }
#endif
}

static void
tk_command (const char *cmd, int from_tty)
{
  int retval;
  std::string result;
  gdbtk_interp *interp = gdbtk_get_interp ();

  /* Catch case of no argument, since this will make the tcl interpreter
     dump core. */
  if (cmd == NULL)
    error_no_arg ("tcl command to interpret");

  retval = Tcl_Eval (interp->tcl, cmd);
  result = Tcl_GetStringResult (interp->tcl);

  if (retval != TCL_OK)
    error ("%s", result.c_str ());

  printf_unfiltered ("%s\n", result.c_str ());
}

static void
view_command (const char *args, int from_tty)
{
  if (args != NULL)
    {
      gdbtk_interp *interp = gdbtk_get_interp ();
      std::string script = string_printf (
	"[lindex [ManagedWin::find SrcWin] 0] location BROWSE_TAG [gdb_loc %s]",
	args);

      if (Tcl_Eval (interp->tcl, script.c_str ()) != TCL_OK)
	{
	  Tcl_Obj *obj = Tcl_GetObjResult (interp->tcl);
	  error ("%s", Tcl_GetStringFromObj (obj, NULL));
	}
    }
  else
    error ("Argument required (location to view)");
}
