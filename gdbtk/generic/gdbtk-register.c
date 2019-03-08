/* Tcl/Tk command definitions for Insight - Registers
   Copyright (C) 2001-2019 Free Software Foundation, Inc.

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
#include "frame.h"
#include "regcache.h"
#include "reggroups.h"
#include "value.h"
#include "target.h"
#include <string.h>
#include "language.h"
#include "valprint.h"
#include "arch-utils.h"

#include <tcl.h>
#include "gdbtk.h"
#include "gdbtk-cmds.h"
#include "gdbtk-interp.h"


/* Extended reg_buffer class to handle register comparison, types & formats. */

class gdbtk_reg_buffer : public reg_buffer
{
public:
  gdbtk_reg_buffer (gdbarch *gdbarch);

  int num_registers () const
  {
    return gdbarch_num_regs (arch ()) + gdbarch_num_pseudo_regs (arch ());
  }

  bool changed_p (int regnum);
  int get_format (int regnum) const
  {
    return m_format[regnum];
  }
  void set_format (int regnum, int fmt)
  {
    m_format[regnum] = fmt;
  }
  struct type *get_type (int regnum) const
  {
    return m_type[regnum];
  }
  void set_type (int regnum, struct type *regtype)
  {
    m_type[regnum] = regtype;
  }

protected:
  std::vector<int> m_format;
  std::vector<struct type *> m_type;
};

/* Argument passed to our register-mapping functions */
typedef union
{
  int integer;
  void *ptr;
} map_arg;

/* Type of our mapping functions */
typedef void (*map_func)(int, map_arg);

static int gdb_register_info (ClientData, Tcl_Interp *, int, Tcl_Obj **);
static void get_register (int, map_arg);
static void get_register_name (int, map_arg);
static void get_register_size (int, map_arg);
static void get_register_types (int regnum, map_arg);
static void get_register_collectable (int regnum, map_arg);
static int map_arg_registers (Tcl_Interp *, int, Tcl_Obj **,
			      map_func, map_arg);
static void register_changed_p (int, map_arg);
static int setup_architecture_data (void);
static int gdb_regformat (ClientData, Tcl_Interp *, int, Tcl_Obj **);
static int gdb_reggroup (ClientData, Tcl_Interp *, int, Tcl_Obj **);
static int gdb_reggrouplist (ClientData, Tcl_Interp *, int, Tcl_Obj **);
static int gdb_regspecial (ClientData, Tcl_Interp *, int, Tcl_Obj **);


static gdbtk_reg_buffer *registers = NULL;


gdbtk_reg_buffer::gdbtk_reg_buffer (gdbarch *gdbarch)
    : reg_buffer (gdbarch, true)
{
  m_format.resize (num_registers (), 0);
  m_type.resize (num_registers (), NULL);
}

bool gdbtk_reg_buffer::changed_p (int regnum)
{
  bool changed = false;

  if (target_has_registers)
    {
      register_status regstatus = REG_VALID;
      gdb_byte *regbuf = register_buffer (regnum);
      int regsize = register_size (arch (), regnum);
      struct value *val = get_frame_register_value (get_selected_frame (NULL),
                                                    regnum);

      if (!val || value_optimized_out (val) || !value_entirely_available (val))
        regstatus = REG_UNAVAILABLE;
      changed = regstatus != m_register_status[regnum];
      if (!changed && regstatus == REG_VALID)
        changed = memcmp (regbuf, value_contents_all (val), regsize) != 0;
      if (changed)
        {
          m_register_status[regnum] = regstatus;
          if (regstatus == REG_VALID)
            memcpy (regbuf, value_contents_all (val), regsize);
          else
            memset (regbuf, 0, regsize);
        }
    }

  return changed;
}


int
Gdbtk_Register_Init (Tcl_Interp *interp)
{
  Tcl_CreateObjCommand (interp, "gdb_reginfo", gdbtk_call_wrapper,
                        (ClientData) gdb_register_info, NULL);
  Tcl_CreateObjCommand (interp, "gdb_reg_arch_changed", gdbtk_call_wrapper,
			(ClientData) setup_architecture_data, NULL);

  /* Register/initialize any architecture specific data */
  setup_architecture_data ();

  return TCL_OK;
}

/* This implements the tcl command "gdb_reginfo".
 * It returns the requested information about registers.
 *
 * Tcl Arguments:
 *    OPTION    - "changed", "name", "size", "value", "type", "format",
 *                "grouplist", "group", "collectable", "special"  (see below)
 *    REGNUM(S) - the register(s) for which info is requested
 *
 * Tcl Result:
 *    The requested information
 *
 * Options:
 * changed
 *    Returns a list of registers whose values have changed since the
 *    last time the proc was called.
 *
 *    usage: gdb_reginfo changed [regnum0, ..., regnumN]
 *
 * name
 *    Return a list containing the names of the registers whose numbers
 *    are given by REGNUM ... .  If no register numbers are given, return
 *    all the registers' names.
 *
 *    usage: gdb_reginfo name [-numbers] [regnum0, ..., regnumN]
 *
 *    Note that some processors have gaps in the register numberings:
 *    even if there is no register numbered N, there may still be a
 *    register numbered N+1.  So if you call gdb_regnames with no
 *    arguments, you can't assume that the N'th element of the result is
 *    register number N.
 *
 *    Given the -numbers option, gdb_regnames returns, not a list of names,
 *    but a list of pairs {NAME NUMBER}, where NAME is the register name,
 *    and NUMBER is its number.
 *
 * size
 *    Returns the raw size of the register(s) in bytes.
 *
 *    usage: gdb_reginfo size [regnum0, ..., regnumN]
 *
 * value
 *    Returns a list of register values.
 *
 *    usage: gdb_reginfo value [regnum0, ..., regnumN]
 *
 * type
 *    Returns a list of valid types for a register.
 *    Normally this will be only one type, except for SIMD and other
 *    special registers.
 *    Each type is represented as a list of 3 elements:
 *    - The type name
 *    - The core address (as an hexadecimal string) of the type structure.
 *    - "float" if it is a floating point type, else "int".
 *
 *    usage: gdb_reginfo type regnum
 *
 * format
 *    Sets the format for a register.
 *    This is necessary to allow "gdb_reginfo value" to return a list
 *    of registers and values.
 *
 *    usage: gdb_reginfo format regno typeaddr format_char
 *
 * grouplist
 *    Returns a list containing the names of the register groups for the
 *    current architecture.
 *
 *    usage: gdb_reginfo grouplist
 *
 * group
 *    Returns a list of the register names in a group.
 *
 *    usage: gdb_reginfo group groupname
 *
 * collectable
 *    Returns a list of flags indicating if register is collectable or not.
 *
 *    usage: gdb_reginfo collectable [regnum0, ..., regnumN]
 *
 * special
 *    Returns a list of special register numbers.
 *
 *    usage: gdb_reginfo special [sp | pc | ps] ...
 */
static int
gdb_register_info (ClientData clientData, Tcl_Interp *interp, int objc,
                   Tcl_Obj **objv)
{
  int index;
  map_arg arg;
  map_func func;
  static const char *commands[] = {"changed", "name", "size", "value", "type",
                                   "format", "group", "grouplist",
                                   "collectable", "special", NULL};
  enum commands_enum { REGINFO_CHANGED, REGINFO_NAME, REGINFO_SIZE,
                       REGINFO_VALUE, REGINFO_TYPE, REGINFO_FORMAT,
                       REGINFO_GROUP, REGINFO_GROUPLIST, REGINFO_COLLECTABLE,
                       REGINFO_SPECIAL };

  if (objc < 2)
    {
      Tcl_WrongNumArgs (interp, 1, objv, "changed|name|size|value|type|format|group|grouplist|collectable|special [regnum1 ... regnumN]");
      return TCL_ERROR;
    }

  if (Tcl_GetIndexFromObj (interp, objv[1], commands, "options", 0,
  			   &index) != TCL_OK)
    {
      result_ptr->flags |= GDBTK_IN_TCL_RESULT;
      return TCL_ERROR;
    }

  /* Skip the option */
  objc -= 2;
  objv += 2;

  arg.integer = 0;
  arg.ptr = NULL;

  switch ((enum commands_enum) index)
    {
    case REGINFO_CHANGED:
      func = register_changed_p;
      break;

    case REGINFO_NAME:
      {
	int len;
	char *s = Tcl_GetStringFromObj (objv[0], &len);
	if (objc != 0 && strncmp (s, "-numbers", len) == 0)
	  {
	    arg.integer = 1;
	    objc--;
	    objv++;
	  }

	func = get_register_name;
      }
      break;

    case REGINFO_SIZE:
      func = get_register_size;
      break;

    case REGINFO_VALUE:
      func = get_register;
      break;

    case REGINFO_TYPE:
      func = get_register_types;
      break;

    case REGINFO_COLLECTABLE:
      func = get_register_collectable;
      break;

    case REGINFO_FORMAT:
      return gdb_regformat (clientData, interp, objc, objv);

    case REGINFO_GROUP:
      return gdb_reggroup (clientData, interp, objc, objv);

    case REGINFO_GROUPLIST:
      return gdb_reggrouplist (clientData, interp, objc, objv);

    case REGINFO_SPECIAL:
      return gdb_regspecial (clientData, interp, objc, objv);

    default:
      return TCL_ERROR;
    }

  return map_arg_registers (interp, objc, objv, func, arg);
}

static void
get_register_size (int regnum, map_arg arg)
{
  gdbtk_interp *interp = gdbtk_get_interp ();

  Tcl_ListObjAppendElement (interp->tcl, result_ptr->obj_ptr,
			    Tcl_NewIntObj (register_size (get_current_arch (),
							  regnum)));
}

static void
get_register_collectable (int regnum, map_arg arg)
{
  int iscollectable = 1;
  gdbtk_interp *interp = gdbtk_get_interp ();

  if (regnum >= gdbarch_num_regs (get_current_arch ()))
    iscollectable = gdbarch_ax_pseudo_register_collect_p (get_current_arch ());

  Tcl_ListObjAppendElement (interp->tcl, result_ptr->obj_ptr,
			    Tcl_NewIntObj (iscollectable));
}

/* returns a list of valid types for a register */
/* Normally this will be only one type, except for SIMD and other */
/* special registers. */

static void
get_register_types (int regnum, map_arg arg)
{
  gdbtk_interp *interp = gdbtk_get_interp ();
  struct type *reg_vtype;
  int i,n;

  reg_vtype = register_type (get_current_arch (), regnum);

  if (TYPE_CODE (reg_vtype) == TYPE_CODE_UNION)
    {
      n = TYPE_NFIELDS (reg_vtype);
      /* limit to 16 types */
      if (n > 16)
	n = 16;

      for (i = 0; i < n; i++)
	{
	  Tcl_Obj *ar[3], *list;

	  ar[0] = Tcl_NewStringObj (TYPE_FIELD_NAME (reg_vtype, i), -1);
	  ar[1] = Tcl_NewStringObj (host_address_to_string (
                                     TYPE_FIELD_TYPE (reg_vtype, i)), -1);
	  if (TYPE_CODE (TYPE_FIELD_TYPE (reg_vtype, i)) == TYPE_CODE_FLT)
	    ar[2] = Tcl_NewStringObj ("float", -1);
	  else
	    ar[2] = Tcl_NewStringObj ("int", -1);
	  list = Tcl_NewListObj (3, ar);
	  Tcl_ListObjAppendElement (interp->tcl,
                                    result_ptr->obj_ptr, list);
	}
    }
  else
    {
      Tcl_Obj *ar[3], *list;

      ar[0] = Tcl_NewStringObj (TYPE_NAME(reg_vtype), -1);
      ar[1] = Tcl_NewStringObj (host_address_to_string (reg_vtype), -1);
      if (TYPE_CODE (reg_vtype) == TYPE_CODE_FLT)
	ar[2] = Tcl_NewStringObj ("float", -1);
      else
	ar[2] = Tcl_NewStringObj ("int", -1);
      list = Tcl_NewListObj (3, ar);
      Tcl_ListObjAppendElement (interp->tcl, result_ptr->obj_ptr, list);
    }
}


static void
get_register (int regnum, map_arg arg)
{
  struct type *reg_vtype;
  int format;
  string_file stb;
  struct gdbarch *gdbarch;
  struct value *val;
  struct frame_info *frame;

  format = registers->get_format (regnum);
  if (format == 0)
    format = 'x';

  reg_vtype = registers->get_type (regnum);
  if (reg_vtype == NULL)
    reg_vtype = register_type (get_current_arch (), regnum);

  if (!target_has_registers)
    {
      if (result_ptr->flags & GDBTK_MAKES_LIST)
	Tcl_ListObjAppendElement (NULL, result_ptr->obj_ptr, Tcl_NewStringObj ("", -1));
      else
	Tcl_SetStringObj (result_ptr->obj_ptr, "", -1);
      return;
    }

  frame = get_selected_frame (NULL);
  gdbarch = get_frame_arch (frame);
  val = get_frame_register_value (frame, regnum);

  if (value_optimized_out (val))
    {
      Tcl_ListObjAppendElement (NULL, result_ptr->obj_ptr,
				Tcl_NewStringObj ("Optimized out", -1));
      return;
    }

  if (format == 'r')
    {
      /* shouldn't happen. raw format is deprecated */
      int j;
      char *ptr, buf[1024];
      const gdb_byte *valaddr = value_contents_for_printing (val);

      strcpy (buf, "0x");
      ptr = buf + 2;
      for (j = 0; j < register_size (gdbarch, regnum); j++)
	{
	  int idx = ((gdbarch_byte_order (gdbarch) == BFD_ENDIAN_BIG)
		     ? j : register_size (gdbarch, regnum) - 1 - j);
	  sprintf (ptr, "%02x", (unsigned char) valaddr[idx]);
	  ptr += 2;
	}
      fputs_unfiltered (buf, &stb);
    }
  else
    {
      struct value_print_options opts;

      get_formatted_print_options (&opts, format);
      opts.deref_ref = 1;
      opts.prettyformat = Val_prettyformat_default;
      val_print (reg_vtype,
		 value_embedded_offset (val), 0,
		 &stb, 0, val, &opts, current_language);
    }

  if (result_ptr->flags & GDBTK_MAKES_LIST)
    Tcl_ListObjAppendElement (NULL, result_ptr->obj_ptr, Tcl_NewStringObj (stb.data (), -1));
  else
    Tcl_SetStringObj (result_ptr->obj_ptr, stb.data (), -1);
}

static void
get_register_name (int regnum, map_arg arg)
{
  /* Non-zero if the caller wants the register numbers, too.  */
  int numbers = arg.integer;
  Tcl_Obj *name
    = Tcl_NewStringObj (gdbarch_register_name (get_current_arch (), regnum), -1);
  Tcl_Obj *elt;

  if (numbers)
    {
      /* Build a tuple of the form "{REGNAME NUMBER}", and append it to
	 our result.  */
      Tcl_Obj *array[2];

      array[0] = name;
      array[1] = Tcl_NewIntObj (regnum);
      elt = Tcl_NewListObj (2, array);
    }
  else
    elt = name;

  Tcl_ListObjAppendElement (NULL, result_ptr->obj_ptr, elt);
}

/* This is a sort of mapcar function for operations on registers */

static int
map_arg_registers (Tcl_Interp *interp, int objc, Tcl_Obj **objv,
		   map_func func, map_arg arg)
{
  int regnum, numregs;

  /* Note that the test for a valid register must include checking the
     gdbarch_register_name because gdbarch_num_regs may be allocated for
     the union of the register sets within a family of related processors.
     In this case, some entries of gdbarch_register_name will change
     depending upon the particular processor being debugged.  */

  numregs = (gdbarch_num_regs (get_current_arch ())
	     + gdbarch_num_pseudo_regs (get_current_arch ()));

  if (objc == 0)		/* No args, just do all the regs */
    {
      result_ptr->flags |= GDBTK_MAKES_LIST;
      for (regnum = 0; regnum < numregs; regnum++)
	{
	  if (gdbarch_register_name (get_current_arch (), regnum) == NULL
	      || *(gdbarch_register_name (get_current_arch (), regnum)) == '\0')
	    continue;
	  func (regnum, arg);
	}
      return TCL_OK;
    }

  if (objc == 1)
    if (Tcl_ListObjGetElements (interp, *objv, &objc, &objv ) != TCL_OK)
      return TCL_ERROR;

  if (objc > 1)
    result_ptr->flags |= GDBTK_MAKES_LIST;

  /* Else, list of register #s, just do listed regs */
  for (; objc > 0; objc--, objv++)
    {
      if (Tcl_GetIntFromObj (NULL, *objv, &regnum) != TCL_OK)
	{
	  result_ptr->flags |= GDBTK_IN_TCL_RESULT;
	  return TCL_ERROR;
	}

      if (regnum >= 0  && regnum < numregs)
	func (regnum, arg);
      else
	{
	  Tcl_SetStringObj (result_ptr->obj_ptr, "bad register number", -1);
	  return TCL_ERROR;
	}
    }
  return TCL_OK;
}

static void
register_changed_p (int regnum, map_arg arg)
{
  gdb_assert (regnum < registers->num_registers ());

  if (registers->changed_p (regnum))
    Tcl_ListObjAppendElement (NULL,
                              result_ptr->obj_ptr, Tcl_NewIntObj (regnum));
}

static int
setup_architecture_data (void)
{
  if (registers)
    delete registers;

  registers = new gdbtk_reg_buffer (target_gdbarch ());
  return TCL_OK;
}

/* gdb_regformat sets the format for a register */
/* This is necessary to allow "gdb_reginfo value" to return a list */
/* of registers and values. */
/* Usage: gdb_reginfo format regno typeaddr format */

static int
gdb_regformat (ClientData clientData, Tcl_Interp *interp,
	       int objc, Tcl_Obj **objv)
{
  int fm, regno, numregs;
  struct type *type;

  if (objc != 3)
    {
      Tcl_WrongNumArgs (interp, 0, objv, "gdb_reginfo regno type format");
      return TCL_ERROR;
    }

  if (Tcl_GetIntFromObj (interp, objv[0], &regno) != TCL_OK)
    return TCL_ERROR;

#ifdef _WIN64
  type = (struct type *)strtoll (Tcl_GetStringFromObj (objv[1], NULL), NULL, 16);
#else
  type = (struct type *)strtol (Tcl_GetStringFromObj (objv[1], NULL), NULL, 16);
#endif

  fm = (int)*(Tcl_GetStringFromObj (objv[2], NULL));

  numregs = (gdbarch_num_regs (target_gdbarch ())
	     + gdbarch_num_pseudo_regs (target_gdbarch ()));
  gdb_assert (numregs == registers->num_registers ());
  if (regno >= numregs)
    {
      gdbtk_set_result (interp, "Register number %d too large", regno);
      return TCL_ERROR;
    }

  registers->set_format (regno, fm);
  registers->set_type (regno, type);

  return TCL_OK;
}


/* gdb_reggrouplist returns the names of the register groups */
/* for the current architecture. */
/* Usage: gdb_reginfo grouplist */

static int
gdb_reggrouplist (ClientData clientData, Tcl_Interp *interp,
		  int objc, Tcl_Obj **objv)
{
  struct reggroup *group;

  if (objc != 0)
    {
      Tcl_WrongNumArgs (interp, 0, objv, "gdb_reginfo grouplist");
      return TCL_ERROR;
    }

  for (group = reggroup_next (get_current_arch (), NULL);
       group != NULL;
       group = reggroup_next (get_current_arch (), group))
    {
      if (reggroup_type (group) == USER_REGGROUP)
	Tcl_ListObjAppendElement (NULL, result_ptr->obj_ptr, Tcl_NewStringObj (reggroup_name (group), -1));
    }
  return TCL_OK;
}


/* gdb_reggroup returns the names of the registers in a group. */
/* Usage: gdb_reginfo group groupname */

static int
gdb_reggroup (ClientData clientData, Tcl_Interp *interp,
	      int objc, Tcl_Obj **objv)
{
  struct reggroup *group;
  char *groupname;
  int regnum, num;

  if (objc != 1)
    {
      Tcl_WrongNumArgs (interp, 0, objv, "gdb_reginfo group groupname");
      return TCL_ERROR;
    }

  groupname = Tcl_GetStringFromObj (objv[0], NULL);
  if (groupname == NULL)
    {
      gdbtk_set_result (interp, "could not read groupname");
      return TCL_ERROR;
    }

  for (group = reggroup_next (get_current_arch (), NULL);
       group != NULL;
       group = reggroup_next (get_current_arch (), group))
    {
      if (strcmp (groupname, reggroup_name (group)) == 0)
	break;
    }

  if (group == NULL)
    return TCL_ERROR;

  num = (gdbarch_num_regs (get_current_arch ())
	 + gdbarch_num_pseudo_regs (get_current_arch ()));
  for (regnum = 0; regnum < num; regnum++)
    {
      if (gdbarch_register_reggroup_p (get_current_arch (), regnum, group))
	Tcl_ListObjAppendElement (NULL, result_ptr->obj_ptr, Tcl_NewIntObj (regnum));
    }
  return TCL_OK;
}

static int
gdb_regspecial (ClientData clientData, Tcl_Interp *interp,
		  int objc, Tcl_Obj **objv)
{
  for (; objc; objc--)
    {
      char *s = Tcl_GetStringFromObj (*objv++, NULL);
      int regnum;

      if (!strcmp (s, "sp"))
        regnum = gdbarch_sp_regnum (get_current_arch ());
      else if (!strcmp (s, "pc"))
        regnum = gdbarch_pc_regnum (get_current_arch ());
      else if (!strcmp (s, "ps"))
        regnum = gdbarch_ps_regnum (get_current_arch ());
      else
        {
          gdbtk_set_result (interp, "Invalid special register %s", s);
          return TCL_ERROR;
        }

      Tcl_ListObjAppendElement (NULL, result_ptr->obj_ptr,
                                Tcl_NewIntObj (regnum));
    }
  return TCL_OK;
}
