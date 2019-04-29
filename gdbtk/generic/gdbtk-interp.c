/* Insight Definitions for GDB, the GNU debugger.
   Written by Keith Seitz <kseitz@sources.redhat.com>

   Copyright (C) 2003-2019 Free Software Foundation, Inc.

   This file is part of Insight.

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
#include "interps.h"
#include "target.h"
#include "ui-file.h"
#include "ui-out.h"
#include "cli-out.h"
#include <string.h>
#include "cli/cli-cmds.h"
#include "cli/cli-decode.h"
#include "exceptions.h"
#include "event-loop.h"
#include "top.h"

#include "tcl.h"
#include "tk.h"
#include "gdbtk.h"
#include "gdbtk-interp.h"

#ifdef __MINGW32__
# define WIN32_LEAN_AND_MEAN
# include <windows.h>
#endif


static void hack_disable_interpreter_exec (const char *, int);


/* See note in gdbtk_interp::init */
static void
hack_disable_interpreter_exec (const char *args, int from_tty)
{
  error ("interpreter-exec not available when running Insight");
}

static void
gdbtk_do_const_cfunc (struct cmd_list_element *c,
                      const char *args, int from_tty)
{
  c->function.const_cfunc (args, from_tty);
}

gdbtk_interp::gdbtk_interp (const char * name): interp (name)
{
  _stdout = NULL;
  _stderr = NULL;
  _stdlog = NULL;
  _stdlog = NULL;
  _stdtarg = NULL;
  _stdtargin = NULL;
  uiout = NULL;

  tcl = NULL;
}

gdbtk_interp::~gdbtk_interp()
{
 if (tcl)
   {
     Tcl_DeleteInterp (tcl);
     gdbtk_uninstall_notifier ();
     tcl = NULL;
   }
}

void
gdbtk_interp::init (bool top_level)
{
  /* Disable interpreter-exec. It causes us big trouble right now. */
  struct cmd_list_element *cmd = NULL;
  struct cmd_list_element *alias = NULL;
  struct cmd_list_element *prefix = NULL;

  _stdout = gdbtk_fileopen ();
  _stderr = gdbtk_fileopen ();
  _stdlog = gdbtk_fileopen ();
  _stdtarg = gdbtk_fileopen ();
  _stdtargin = gdbtk_fileopen ();
  uiout = cli_out_new (_stdout),

  gdbtk_init (this);

  if (lookup_cmd_composition ("interpreter-exec", &alias, &prefix, &cmd))
    {
      /* Change command processor function. */
      cmd->func = gdbtk_do_const_cfunc;
      cmd->function.const_cfunc = hack_disable_interpreter_exec;
    }
}

void
gdbtk_interp::resume ()
{
  static int started = 0;

  gdbtk_add_hooks ();

  gdb_stdout = _stdout;
  gdb_stderr = _stderr;
  gdb_stdlog = _stdlog;
  gdb_stdtarg = _stdtarg;
  gdb_stdtargin = _stdtargin;

  /* 2003-02-11 keiths: We cannot actually source our main Tcl file in
     our interpreter's init function because any errors that may
     get generated will go to the wrong gdb_stderr. Instead of hacking
     our interpreter init function to force gdb_stderr to our ui_file,
     we defer sourcing the startup file until now, when gdb is ready
     to let our interpreter run. */
  if (!started)
    {
      started = 1;
      gdbtk_source_start_file (this);
    }
}

void
gdbtk_interp::suspend ()
{
}

gdb_exception
gdbtk_interp::exec (const char *command_str)
{
  return gdb_exception ();
}

/* This function is called before entering gdb's internal command loop.
   This is the last chance to do anything before entering the event loop. */

void
gdbtk_interp::pre_command_loop ()
{
  /* We no longer want to use stdin as the command input stream: disable
     events from stdin. */
  main_ui->input_fd = -1;

  if (Tcl_Eval (tcl, "gdbtk_tcl_preloop") != TCL_OK)
    {
      const char *msg;

      /* Force errorInfo to be set up propertly.  */
      Tcl_AddErrorInfo (tcl, "");

      msg = Tcl_GetVar (tcl, "errorInfo", TCL_GLOBAL_ONLY);
#ifdef _WIN32
      MessageBox (NULL, msg, NULL, MB_OK | MB_ICONERROR | MB_TASKMODAL);
#else
      fputs_unfiltered (msg, gdb_stderr);
#endif
    }

#ifdef _WIN32
  close_bfds ();
#endif
}

ui_out *
gdbtk_interp::interp_ui_out ()
{
  return uiout;
}

void
gdbtk_interp::set_logging (ui_file_up logfile, bool logging_redirect)
{
}

/* Get insight's current interpreter. */
gdbtk_interp *
gdbtk_get_interp (void)
{
  if (!current_interp_named_p(INTERP_INSIGHT))
    throw_error (NOT_FOUND_ERROR, "current intepreter is not insight's");
  return (gdbtk_interp *) current_interpreter ();
}

/* Factory for GUI interpreter. */
static struct interp *
gdbtk_interp_factory (const char *name)
{
  return new gdbtk_interp (name);
}

void
initialize_gdbtk_interp (void)
{
  /* Does not run in target-async mode. */
  target_async_permitted = 0;
  interp_factory_register (INTERP_INSIGHT, gdbtk_interp_factory);
}
