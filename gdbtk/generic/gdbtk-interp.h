/* Tcl/Tk interface routines header file.
   Copyright (C) 1994-2019 Free Software Foundation, Inc.

   This file is part of GDB.  It contains the public data that is shared between
   the gdbtk startup code and the gdbtk commands.

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

#ifndef _GDBTK_INTERP_H
#define _GDBTK_INTERP_H

#include "defs.h"
#include "interps.h"
#include "ui-file.h"
#include "ui-out.h"


/* The gdb interpreter. */

class gdbtk_interp final : public interp
{
public:
  gdbtk_interp (const char * name);
  ~gdbtk_interp();

  void init (bool top_level) override;
  void resume () override;
  void suspend () override;
  gdb_exception exec (const char * command_str) override;
  ui_out *interp_ui_out () override;
  void set_logging (ui_file_up logfile, bool logging_redirect) override;
  void pre_command_loop () override;

  ui_file *_stdout;
  ui_file *_stderr;
  ui_file *_stdlog;
  ui_file *_stdtarg;
  ui_file *_stdtargin;
  ui_out *uiout;

  Tcl_Interp *tcl;
};

extern gdbtk_interp *gdbtk_get_interp (void);
extern void initialize_gdbtk_interp (void);

#endif /* _GDBTK_INTERP_H */
