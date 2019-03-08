/* Startup code for Insight.

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
#include "top.h"
#include "annotate.h"
#include "cli/cli-decode.h"
#include "observable.h"
#include "gdbthread.h"
#include "event-loop.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* tcl header files includes varargs.h unless HAS_STDARG is defined,
   but gdb uses stdarg.h, so make sure HAS_STDARG is defined.  */
#define HAS_STDARG 1

#include <tcl.h>
#include <tk.h>
#include "guitcl.h"
#include "gdbtk.h"
#include "gdbtk-interp.h"

#include <signal.h>
#include <fcntl.h>
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#include <sys/time.h>

#include <string.h>
#include "dis-asm.h"
#include "gdbcmd.h"


volatile bool gdbtk_in_write = false;

/* Set by gdb_stop, this flag informs x_event to tell its caller
   that it should forcibly detach from the target. */
int gdbtk_force_detach = 0;

/* From gdbtk-bp.c */
extern void gdbtk_create_breakpoint (struct breakpoint *);
extern void gdbtk_delete_breakpoint (struct breakpoint *);
extern void gdbtk_modify_breakpoint (struct breakpoint *);

static void gdbtk_architecture_changed (struct gdbarch *);
static void gdbtk_trace_find (int tfnum, int tpnum);
static void gdbtk_trace_start_stop (int, int);
static void gdbtk_attach (void);
static void gdbtk_detach (void);
static void gdbtk_file_changed (const char *);
static void gdbtk_exec_file_display (const char *);
static void gdbtk_call_command (struct cmd_list_element *, const char *, int);
static ptid_t gdbtk_wait (ptid_t, struct target_waitstatus *, int);
int x_event (int);
static int gdbtk_query (const char *, va_list);
static void gdbtk_warning (const char *, va_list);
static char *gdbtk_readline (const char *);
static void gdbtk_readline_begin (const char *format,...);
static void gdbtk_readline_end (void);
static void gdbtk_pre_add_symbol (const char *);
static void gdbtk_print_frame_info (struct symtab *, int, int, int);
static void gdbtk_post_add_symbol (void);
static void gdbtk_register_changed (struct frame_info *frame, int regno);
static void gdbtk_memory_changed (struct inferior *inferior, CORE_ADDR addr,
				  ssize_t len, const bfd_byte *data);
static void gdbtk_context_change (int);
static void gdbtk_error_begin (void);
void report_error (void);
static void gdbtk_annotate_signal (void);
static void gdbtk_param_changed (const char *, const char *);

/* I/O stream for gdbtk. */

class gdbtk_file : public ui_file
{
public:
  virtual long read (char *buf, long length_buf) override;
  virtual void write (const char *buf, long length_buf) override;
};

static int gdbtk_load_hash (const char *, unsigned long);

static ptid_t gdbtk_ptid;

/*
 * gdbtk_add_hooks - add all the hooks to gdb.  This will get called by the
 * startup code to fill in the hooks needed by core gdb.
 */

void
gdbtk_add_hooks (void)
{
  /* Gdb observers */
  gdb::observers::breakpoint_created.attach (gdbtk_create_breakpoint);
  gdb::observers::breakpoint_modified.attach (gdbtk_modify_breakpoint);
  gdb::observers::breakpoint_deleted.attach (gdbtk_delete_breakpoint);
  gdb::observers::architecture_changed.attach (gdbtk_architecture_changed);
  gdb::observers::memory_changed.attach (gdbtk_memory_changed);
  gdb::observers::command_param_changed.attach (gdbtk_param_changed);
  gdb::observers::register_changed.attach (gdbtk_register_changed);
  gdb::observers::traceframe_changed.attach (gdbtk_trace_find);

  /* Hooks */
  deprecated_call_command_hook = gdbtk_call_command;
  deprecated_readline_begin_hook = gdbtk_readline_begin;
  deprecated_readline_hook = gdbtk_readline;
  deprecated_readline_end_hook = gdbtk_readline_end;

  deprecated_print_frame_info_listing_hook = gdbtk_print_frame_info;
  deprecated_query_hook = gdbtk_query;
  deprecated_warning_hook = gdbtk_warning;

  deprecated_interactive_hook = gdbtk_interactive;
  deprecated_target_wait_hook = gdbtk_wait;
  deprecated_ui_load_progress_hook = gdbtk_load_hash;

  deprecated_ui_loop_hook = x_event;
  deprecated_pre_add_symbol_hook = gdbtk_pre_add_symbol;
  deprecated_post_add_symbol_hook = gdbtk_post_add_symbol;
  deprecated_file_changed_hook = gdbtk_file_changed;
  specify_exec_file_hook (gdbtk_exec_file_display);

  deprecated_attach_hook            = gdbtk_attach;
  deprecated_detach_hook            = gdbtk_detach;

  deprecated_context_hook = gdbtk_context_change;

  deprecated_error_begin_hook = gdbtk_error_begin;

  deprecated_annotate_signal_hook = gdbtk_annotate_signal;
  deprecated_annotate_signalled_hook = gdbtk_annotate_signal;
}

/* These control where to put the gdb output which is created by
   {f}printf_{un}filtered and friends.  gdbtk_file::write is the lowest
   level of these routines and capture all output from the rest of
   GDB.

   The reason to use the result_ptr rather than the tcl interpreter's result
   directly is so that a call_wrapper invoked function can preserve its result
   across calls into Tcl which might be made in the course of the function's
   execution.

   * result_ptr->obj_ptr is where to accumulate the result.
   * GDBTK_TO_RESULT flag means the output goes to the gdbtk_tcl_fputs proc
   instead of to the result_ptr.
   * GDBTK_MAKES_LIST flag means add to the result as a list element.

*/

gdbtk_result *result_ptr = NULL;

/* If you want to restore an old value of result_ptr whenever cleanups
   are run, pass this function to make_cleanup, along with the value
   of result_ptr you'd like to reinstate.  */
void
gdbtk_restore_result_ptr (void *old_result_ptr)
{
  result_ptr = (gdbtk_result *) old_result_ptr;
}

/* This allows you to Tcl_Eval a tcl command which takes
   a command word, and then a single argument. */
int
gdbtk_two_elem_cmd (const char *cmd_name, const char *argv1)
{
  char *command;
  int result, flags_ptr, arg_len, cmd_len;
  gdbtk_interp *interp = gdbtk_get_interp ();

  arg_len = Tcl_ScanElement (argv1, &flags_ptr);
  cmd_len = strlen (cmd_name);
  command = (char *) malloc (arg_len + cmd_len + 2);
  strcpy (command, cmd_name);
  strcat (command, " ");

  Tcl_ConvertElement (argv1, command + cmd_len + 1, flags_ptr);

  result = Tcl_Eval (interp->tcl, command);
  if (result != TCL_OK)
    report_error ();
  free (command);
  return result;
}

struct ui_file *
gdbtk_fileopen (void)
{
  return new gdbtk_file;
}

/* This handles input from the gdb console.
 */

long
gdbtk_file::read (char *buf, long sizeof_buf)
{
  int result;
  size_t actual_len;
  gdbtk_interp *interp = gdbtk_get_interp ();

  if (this == gdb_stdtargin)
    {
      result = Tcl_Eval (interp->tcl, "gdbtk_console_read");
      if (result != TCL_OK)
        {
          report_error ();
          actual_len = 0;
          buf[0] = '\0';
          return 0;
        }
      else
        {
          const char *tclResult = Tcl_GetStringResult (interp->tcl);
          actual_len = strlen (tclResult);

          /* Truncate the string if it is too big for the caller's buffer.  */
          if (actual_len >= sizeof_buf)
            actual_len = sizeof_buf - 1;

          memcpy (buf, tclResult, actual_len);
          buf[actual_len] = '\0';
          return actual_len;
        }
    }
  else
    {
      errno = EBADF;
      return 0;
    }
}


/* This handles all the output from gdb.  All the gdb printf_xxx functions
 * eventually end up here.  The output is either passed to the result_ptr
 * where it will go to the result of some gdbtk command, or passed to the
 * Tcl proc gdbtk_tcl_fputs (where it is usually just dumped to the console
 * window.
 *
 * The cases are:
 *
 * 1) result_ptr == NULL - This happens when some output comes from gdb which
 *    is not generated by a command in gdbtk-cmds, usually startup stuff.
 *    In this case we just route the data to gdbtk_tcl_fputs.
 * 2) The GDBTK_TO_RESULT flag is set - The result is supposed to go to Tcl.
 *    We place the data into the result_ptr, either as a string,
 *    or a list, depending whether the GDBTK_MAKES_LIST bit is set.
 * 3) The GDBTK_TO_RESULT flag is unset - We route the data to gdbtk_tcl_fputs
 *    UNLESS it was coming to gdb_stderr.  Then we place it in the result_ptr
 *    anyway, so it can be dealt with.
 *
 * This method only supports text output, so null bytes cannot appear in
 * output data.
 *
 */

void
gdbtk_file::write (const char *buf, long length_buf)
{
  std::string tmp;
  char *ptr;

  if (gdbtk_disable_write || length_buf < 0)
    return;

  gdbtk_in_write = true;
  tmp = std::string(buf, (size_t) length_buf);
  ptr = (char *) tmp.data ();

  if (this == gdb_stdlog)
    gdbtk_two_elem_cmd ("gdbtk_tcl_fputs_log", ptr);
  else if (this == gdb_stdtarg)
    gdbtk_two_elem_cmd ("gdbtk_tcl_fputs_target", ptr);
  else if (result_ptr != NULL)
    {
      if (result_ptr->flags & GDBTK_TO_RESULT)
	{
	  if (result_ptr->flags & GDBTK_MAKES_LIST)
	    Tcl_ListObjAppendElement (NULL, result_ptr->obj_ptr,
				      Tcl_NewStringObj (ptr, -1));
	  else
	    Tcl_AppendToObj (result_ptr->obj_ptr, ptr, -1);
	}
      else if (this == gdb_stderr || result_ptr->flags & GDBTK_ERROR_ONLY)
	{
	  if (result_ptr->flags & GDBTK_ERROR_STARTED)
	    Tcl_AppendToObj (result_ptr->obj_ptr, ptr, -1);
	  else
	    {
	      Tcl_SetStringObj (result_ptr->obj_ptr, ptr, -1);
	      result_ptr->flags |= GDBTK_ERROR_STARTED;
	    }
	}
      else
	{
	  gdbtk_two_elem_cmd ("gdbtk_tcl_fputs", ptr);
	  if (result_ptr->flags & GDBTK_MAKES_LIST)
	    gdbtk_two_elem_cmd ("gdbtk_tcl_fputs", " ");
	}
    }
  else
    {
      gdbtk_two_elem_cmd ("gdbtk_tcl_fputs", ptr);
    }

  gdbtk_in_write = false;
}

/*
 * This gets the current process id in a portable way.
 */

long
gdbtk_getpid(void)
{
  long mypid = -1;
  gdbtk_interp *interp = gdbtk_get_interp ();

  if (Tcl_Eval (interp->tcl, "pid") == TCL_OK)
    {
      Tcl_Obj *pidobj = Tcl_GetObjResult (interp->tcl);

      if (pidobj)
        {
          if (Tcl_GetLongFromObj (interp->tcl, pidobj, &mypid) != TCL_OK)
            mypid = -1;
        }
    }

  return mypid;
}

/*
 * This routes all warnings to the Tcl function "gdbtk_tcl_warning".
 */

static void
gdbtk_warning (const char *warning, va_list args)
{
  /* This procedure may be called in a child process before it has exec'ed.
     In such condition, the X server is no longer reachable and thus the
     warning may not be presented as a dialog.
     Therefore this condition is checked here (using process ids) and
     if detected, vwarning() is called recursively after resetting the hook. */

  if (gdbtk_getpid () != gdbtk_pid)
    {
      struct ui_file *svstderr = gdb_stderr;

      deprecated_warning_hook = NULL;
      gdb_stderr = new stderr_file (stderr);
      vwarning (warning, args);
      gdb_flush (gdb_stderr);
      delete gdb_stderr;

      /* Restore previous values, since if we vforked, global storage is shared
         with parent. */
      gdb_stderr = svstderr;
      deprecated_warning_hook = gdbtk_warning;
    }
  else
    {
      char *buf = xstrvprintf (warning, args);
      gdbtk_two_elem_cmd ("gdbtk_tcl_warning", buf);
      free(buf);
    }
}


/* Error-handling function for all hooks */
/* Hooks are not like tcl functions, they do not simply return */
/* TCL_OK or TCL_ERROR.  Also, the calling function typically */
/* doesn't care about errors in the hook functions.  Therefore */
/* after every hook function, report_error should be called. */
/* report_error can just call Tcl_BackgroundError() which will */
/* pop up a messagebox, or it can silently log the errors through */
/* the gdbtk dbug command.  */

void
report_error (void)
{
  gdbtk_interp *interp = gdbtk_get_interp ();

  TclDebug ('E', Tcl_GetVar (interp->tcl, "errorInfo", TCL_GLOBAL_ONLY));
  /*  Tcl_BackgroundError(interp->tcl); */
}

/*
 * This routes all ignorable warnings to the Tcl function
 * "gdbtk_tcl_ignorable_warning".
 */

void
gdbtk_ignorable_warning (const char *warnclass, const char *warning)
{
  char *buf;
  gdbtk_interp *interp = gdbtk_get_interp ();

  buf = xstrprintf ("gdbtk_tcl_ignorable_warning {%s} {%s}",
		    warnclass, warning);
  if (Tcl_Eval (interp->tcl, buf) != TCL_OK)
    report_error ();
  free(buf);
}

static void
gdbtk_register_changed (struct frame_info *frame, int regno)
{
  gdbtk_interp *interp = gdbtk_get_interp ();

  if (Tcl_Eval (interp->tcl, "gdbtk_register_changed") != TCL_OK)
    report_error ();
}

static void
gdbtk_memory_changed (struct inferior *inferior, CORE_ADDR addr,
		      ssize_t len, const bfd_byte *data)
{
  gdbtk_interp *interp = gdbtk_get_interp ();

  if (Tcl_Eval (interp->tcl, "gdbtk_memory_changed") != TCL_OK)
    report_error ();
}


/* This hook is installed as the deprecated_ui_loop_hook, which is
 * used in several places to keep the gui alive (x_event runs gdbtk's
 * event loop). Users include:
 * - ser-tcp.c in socket reading code
 * - ser-unix.c in serial port reading code
 * - built-in simulators while executing
 *
 * x_event used to be called on SIGIO on the socket to the X server
 * for unix. Unfortunately, Linux does not deliver SIGIO, so we resort
 * to an elaborate scheme to keep the gui alive.
 *
 * For simulators and socket or serial connections on all hosts, we
 * rely on deprecated_ui_loop_hook (x_event) to keep us going. If the
 * user requests a detach (as a result of pressing the stop button --
 * see comments before gdb_stop in gdbtk-cmds.c), it sets the global
 * GDBTK_FORCE_DETACH, which is the value that x_event returns to it's
 * caller. It is up to the caller of x_event to act on this
 * information.
 *
 * For native unix, we simply set an interval timer which calls
 * x_event to allow the debugger to run through the Tcl event
 * loop. See comments before gdbtk_start_timer and gdb_stop_timer
 * in gdbtk.c.
 *
 * For native windows (and a few other targets, like the v850 ICE), we
 * rely on the target_wait loops to call deprecated_ui_loop_hook to
 * keep us alive.  */
int
x_event (int signo)
{
  static volatile int in_x_event = 0;
  static Tcl_Obj *varname = NULL;
  gdbtk_interp *interp = gdbtk_get_interp ();

  /* Do nor re-enter this code or enter it while collecting gdb output. */
  if (in_x_event || gdbtk_in_write)
    return 0;

  /* Also, only do things while the target is running (stops and redraws).
     FIXME: We wold like to at least redraw at other times but this is bundled
     together in the TCL_WINDOW_EVENTS group and we would also process user
     input.  We will have to prevent (unwanted)  user input to be generated
     in order to be able to redraw (removing this test here). */
  if (!running_now)
    return 0;

  in_x_event = 1;
  gdbtk_force_detach = 0;

  /* Process pending events */
  while (Tcl_DoOneEvent (TCL_DONT_WAIT | TCL_ALL_EVENTS) != 0)
    ;

  if (load_in_progress)
    {
      int val;
      if (varname == NULL)
	{
#if TCL_MAJOR_VERSION == 8 && (TCL_MINOR_VERSION < 1 || TCL_MINOR_VERSION > 2)
	  Tcl_Obj *varnamestrobj = Tcl_NewStringObj ("download_cancel_ok", -1);
	  varname = Tcl_ObjGetVar2 (interp->tcl, varnamestrobj,
                                    NULL, TCL_GLOBAL_ONLY);
#else
	  varname = Tcl_GetObjVar2 (interp->tcl, "download_cancel_ok",
                                    NULL, TCL_GLOBAL_ONLY);
#endif
	}
      if (Tcl_GetIntFromObj (interp->tcl, varname, &val) == TCL_OK && val)
	{
	  set_quit_flag ();
#ifdef REQUEST_QUIT
	  REQUEST_QUIT;
#else
	  QUIT;
#endif
	}
    }
  in_x_event = 0;

  return gdbtk_force_detach;
}

/* VARARGS */
static void
gdbtk_readline_begin (const char *format,...)
{
  va_list args;
  char *buf;

  va_start (args, format);
  buf = xstrvprintf (format, args);
  gdbtk_two_elem_cmd ("gdbtk_tcl_readline_begin", buf);
  free(buf);
}

static char *
gdbtk_readline (const char *prompt)
{
  int result;
  gdbtk_interp *interp = gdbtk_get_interp ();

#ifdef _WIN32
  close_bfds ();
#endif

  result = gdbtk_two_elem_cmd ("gdbtk_tcl_readline", prompt);

  if (result == TCL_OK)
    {
      return (xstrdup (Tcl_GetStringResult (interp->tcl)));
    }
  else
    {
      gdb_stdout->puts (Tcl_GetStringResult (interp->tcl));
      gdb_stdout->puts ("\n");
      return (NULL);
    }
}

static void
gdbtk_readline_end (void)
{
  gdbtk_interp *interp = gdbtk_get_interp ();

  if (Tcl_Eval (interp->tcl, "gdbtk_tcl_readline_end") != TCL_OK)
    report_error ();
}

static void
gdbtk_call_command (struct cmd_list_element *cmdblk,
		    const char *arg, int from_tty)
{
  gdbtk_interp *interp = gdbtk_get_interp ();

  running_now = 0;
  if (cmdblk->theclass == class_run || cmdblk->theclass == class_trace)
    {
      int tracerunning = current_trace_status ()->running;

      running_now = 1;
      if (!No_Update)
	Tcl_Eval (interp->tcl, "gdbtk_tcl_busy");
      cmd_func (cmdblk, arg, from_tty);

      /* The above function may return before the target stops running even
         in synchronous mode. Make sure the target is not running by
         monitoring gdb events. */
      while (inferior_ptid != null_ptid &&
             inferior_thread ()->state == THREAD_RUNNING)
        gdb_do_one_event (-1);

      /* Emulate trace start/stop hook. */
      if (tracerunning != current_trace_status ()->running)
        gdbtk_trace_start_stop (current_trace_status ()->running, from_tty);

      running_now = 0;
      if (!No_Update)
	Tcl_Eval (interp->tcl, "gdbtk_tcl_idle");
    }
  else
    cmd_func (cmdblk, arg, from_tty);
}

/* Called after a `set' command succeeds.  Runs the Tcl hook
   `gdb_set_hook' with the full name of the variable (a Tcl list) as
   the first argument and the new value as the second argument.  */

static void
gdbtk_param_changed (const char *param, const char *value)
{
  Tcl_DString cmd;
  char *buffer = NULL;
  gdbtk_interp *interp = gdbtk_get_interp ();

  Tcl_DStringInit (&cmd);
  Tcl_DStringAppendElement (&cmd, "gdbtk_tcl_set_variable");

  Tcl_DStringAppendElement (&cmd, param);
  Tcl_DStringAppendElement (&cmd, value);

  if (Tcl_Eval (interp->tcl, Tcl_DStringValue (&cmd)) != TCL_OK)
    report_error ();

  Tcl_DStringFree (&cmd);

  if (buffer != NULL)
    {
      free(buffer);
    }
}

int
gdbtk_load_hash (const char *section, unsigned long num)
{
  char *buf;
  gdbtk_interp *interp = gdbtk_get_interp ();

  buf = xstrprintf ("Download::download_hash %s %ld", section, num);
  if (Tcl_Eval (interp->tcl, buf) != TCL_OK)
    report_error ();
  free(buf);

  return atoi (Tcl_GetStringResult (interp->tcl));
}


/* This hook is called whenever we are ready to load a symbol file so that
   the UI can notify the user... */
static void
gdbtk_pre_add_symbol (const char *name)
{
  gdbtk_two_elem_cmd ("gdbtk_tcl_pre_add_symbol", name);
}

/* This hook is called whenever we finish loading a symbol file. */
static void
gdbtk_post_add_symbol (void)
{
  gdbtk_interp *interp = gdbtk_get_interp ();

  if (Tcl_Eval (interp->tcl, "gdbtk_tcl_post_add_symbol") != TCL_OK)
    report_error ();
}

/* This hook function is called whenever we want to wait for the
   target.  */

static ptid_t
gdbtk_wait (ptid_t ptid, struct target_waitstatus *ourstatus, int options)
{
  gdbtk_force_detach = 0;
  gdbtk_start_timer ();
  ptid = target_wait (ptid, ourstatus, options);
  gdbtk_stop_timer ();
  gdbtk_ptid = ptid;

  return ptid;
}

/*
 * This handles all queries from gdb.
 * The first argument is a printf style format statement, the rest are its
 * arguments.  The resultant formatted string is passed to the Tcl function
 * "gdbtk_tcl_query".
 * It returns the users response to the query, as well as putting the value
 * in the result field of the Tcl interpreter.
 */

static int
gdbtk_query (const char *query, va_list args)
{
  gdbtk_interp *interp = gdbtk_get_interp ();
  char *buf;
  long val;

  buf = xstrvprintf (query, args);
  gdbtk_two_elem_cmd ("gdbtk_tcl_query", buf);
  free(buf);

  val = atol (Tcl_GetStringResult (interp->tcl));
  return val;
}


static void
gdbtk_print_frame_info (struct symtab *s, int line,
			int stopline, int noerror)
{
  /* Do nothing: just here to disable frame info output to console from gdb. */
}

/*
 * gdbtk_trace_find
 *
 * This is run by the trace_find_command.  arg is the argument that was passed
 * to that command, from_tty is 1 if the command was run from a tty, 0 if it
 * was run from a script.  It runs gdbtk_tcl_tfind_hook passing on these two
 * arguments.
 *
 */

static void
gdbtk_trace_find (int tfnum, int tpnum)
{
  gdbtk_interp *interp = gdbtk_get_interp ();
  Tcl_Obj *cmdObj;

  cmdObj = Tcl_NewListObj (0, NULL);
  Tcl_ListObjAppendElement (interp->tcl, cmdObj,
			    Tcl_NewStringObj ("gdbtk_tcl_trace_find_hook", -1));
  Tcl_ListObjAppendElement (interp->tcl, cmdObj, Tcl_NewIntObj (tfnum));
  Tcl_ListObjAppendElement (interp->tcl, cmdObj, Tcl_NewIntObj (tpnum));
#if TCL_MAJOR_VERSION == 8 && (TCL_MINOR_VERSION < 1 || TCL_MINOR_VERSION > 2)
  if (Tcl_GlobalEvalObj (interp->tcl, cmdObj) != TCL_OK)
    report_error ();
#else
  if (Tcl_EvalObj (interp->tcl, cmdObj, TCL_EVAL_GLOBAL) != TCL_OK)
    report_error ();
#endif
}

/*
 * gdbtk_trace_start_stop
 *
 * This is run by the trace_start_command and trace_stop_command.
 * The START variable determines which, 1 meaning trace_start was run,
 * 0 meaning trace_stop was run.
 *
 */

static void
gdbtk_trace_start_stop (int start, int from_tty)
{
  gdbtk_interp *interp = gdbtk_get_interp ();

  if (start)
    Tcl_GlobalEval (interp->tcl, "gdbtk_tcl_tstart");
  else
    Tcl_GlobalEval (interp->tcl, "gdbtk_tcl_tstop");

}

/* Called when the current thread changes. */
/* gdb_context is linked to the tcl variable "gdb_context_id" */
static void
gdbtk_context_change (int num)
{
  gdb_context = num;
}

/* Called from file_command */
static void
gdbtk_file_changed (const char *filename)
{
  gdbtk_two_elem_cmd ("gdbtk_tcl_file_changed", filename);
}

/* Called from exec_file_command */
static void
gdbtk_exec_file_display (const char *filename)
{
  gdbtk_two_elem_cmd ("gdbtk_tcl_exec_file_display", filename);
}

/* Called from error_begin, this hook is used to warn the gui
   about multi-line error messages */
static void
gdbtk_error_begin (void)
{
  if (result_ptr != NULL)
    result_ptr->flags |= GDBTK_ERROR_ONLY;
}

/* notify GDBtk when a signal occurs */
static void
gdbtk_annotate_signal (void)
{
  gdbtk_interp *interp = gdbtk_get_interp ();
  char *buf;
  struct thread_info *tp;

  /* Inform gui that the target has stopped. This is
     a necessary stop button evil. We don't want signal notification
     to interfere with the elaborate and painful stop button detach
     timeout. */
  Tcl_Eval (interp->tcl, "gdbtk_stop_idle_callback");

  if (inferior_ptid == null_ptid)
    return;

  tp = inferior_thread ();

  buf = xstrprintf ("gdbtk_signal %s {%s}",
	     gdb_signal_to_name (tp->suspend.stop_signal),
	     gdb_signal_to_string (tp->suspend.stop_signal));
  if (Tcl_Eval (interp->tcl, buf) != TCL_OK)
    report_error ();
  free(buf);
}

static void
gdbtk_attach (void)
{
  gdbtk_interp *interp = gdbtk_get_interp ();

  if (Tcl_Eval (interp->tcl,
                "after idle \"update idletasks;gdbtk_attached\"") != TCL_OK)
    {
      report_error ();
    }
}

static void
gdbtk_detach (void)
{
  gdbtk_interp *interp = gdbtk_get_interp ();

  if (Tcl_Eval (interp->tcl, "gdbtk_detached") != TCL_OK)
    {
      report_error ();
    }
}

/* Called from gdbarch_update_p whenever the architecture changes. */
static void
gdbtk_architecture_changed (struct gdbarch *ignore)
{
  gdbtk_interp *interp = gdbtk_get_interp ();

  Tcl_Eval (interp->tcl, "gdbtk_tcl_architecture_changed");
}

ptid_t
gdbtk_get_ptid (void)
{
  return gdbtk_ptid;
}
