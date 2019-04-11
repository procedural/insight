/* Variable user interface layer for GDB, the GNU debugger.
   Copyright (C) 1999-2019 Free Software Foundation, Inc.

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
#include "value.h"
#include <string.h>
#include "varobj.h"
#include "valprint.h"
#include "ui-file.h"
#include "language.h"
#include "exceptions.h"

#include <tcl.h>
#include "gdbtk.h"
#include "gdbtk-cmds.h"
#include "gdbtk-wrapper.h"

/*
 * Public functions defined in this file
 */

int gdb_variable_init (Tcl_Interp *);

/*
 * Private functions defined in this file
 */

/* Entries into this file */

static int gdb_variable_command (ClientData, Tcl_Interp *, int,
				 Tcl_Obj * CONST[]);

static int variable_obj_command (ClientData, Tcl_Interp *, int,
				 Tcl_Obj * CONST[]);

/* Variable object subcommands */

static int variable_create (Tcl_Interp *, int, Tcl_Obj * CONST[]);

static void variable_delete (Tcl_Interp *, struct varobj *, int);

static Tcl_Obj *variable_children (Tcl_Interp *, struct varobj *);

static int variable_format (Tcl_Interp *, int, Tcl_Obj * CONST[],
			    struct varobj *);

static int variable_type (Tcl_Interp *, int, Tcl_Obj * CONST[],
			  struct varobj *);

static int variable_value (Tcl_Interp *, int, Tcl_Obj * CONST[],
			   struct varobj *);

static int variable_print (Tcl_Interp *, int, Tcl_Obj * CONST[],
			   struct varobj *);

static Tcl_Obj *variable_update (Tcl_Interp * interp, struct varobj **var);

/* Helper functions for the above subcommands. */

static void install_variable (Tcl_Interp *, const char *);

static void uninstall_variable (Tcl_Interp *, const char *);

/* String representations of gdb's format codes */
static const char *format_string[] =
  {"natural", "binary", "decimal", "hexadecimal", "octal"};


/* Initialize the variable code. This function should be called once
   to install and initialize the variable code into the interpreter. */
int
gdb_variable_init (Tcl_Interp *interp)
{
  Tcl_Command result;
  static int initialized = 0;

  if (!initialized)
    {
      result = Tcl_CreateObjCommand (interp, "gdb_variable", gdbtk_call_wrapper,
				     (ClientData) gdb_variable_command, NULL);
      if (result == NULL)
	return TCL_ERROR;

      initialized = 1;
    }

  return TCL_OK;
}

/* This function defines the "gdb_variable" command which is used to
   create variable objects. Its syntax includes:

   gdb_variable create
   gdb_variable create NAME
   gdb_variable create -expr EXPR
   gdb_variable create -frame FRAME
   (it will also include permutations of the above options)

   NAME  = name of object to create. If no NAME, then automatically create
   a name
   EXPR  = the gdb expression for which to create a variable. This will
   be the most common usage.
   FRAME = the frame defining the scope of the variable.
*/
static int
gdb_variable_command (ClientData clientData, Tcl_Interp *interp,
		      int objc, Tcl_Obj *CONST objv[])
{
  static const char *commands[] =
    {"create", "list", NULL};
  enum commands_enum
    {
      VARIABLE_CREATE, VARIABLE_LIST
    };
  int index, result;

  if (objc < 2)
    {
      Tcl_WrongNumArgs (interp, 1, objv, "option ?arg...?");
      return TCL_ERROR;
    }

  if (Tcl_GetIndexFromObj (interp, objv[1], commands, "options", 0,
			   &index) != TCL_OK)
    {
      return TCL_ERROR;
    }

  switch ((enum commands_enum) index)
    {
    case VARIABLE_CREATE:
      result = variable_create (interp, objc - 2, objv + 2);
      break;

    default:
      return TCL_ERROR;
    }

  return result;
}

/* This function implements the actual object command for each
   variable object that is created (and each of its children).

   Currently the following commands are implemented:
   - delete        delete this object and its children
   - update        update the variable and its children (root vars only)
   - numChildren   how many children does this object have
   - children      create the children and return a list of their objects
   - name          print out the name of this variable
   - format        query/set the display format of this variable
   - type          get the type of this variable
   - value         get/set the value of this variable
   - print         get the variable value for printing
   - editable      is this variable editable?
*/
static int
variable_obj_command (ClientData clientData, Tcl_Interp *interp,
		      int objc, Tcl_Obj *CONST objv[])
{
  enum commands_enum
    {
      VARIABLE_DELETE,
      VARIABLE_NUM_CHILDREN,
      VARIABLE_CHILDREN,
      VARIABLE_FORMAT,
      VARIABLE_TYPE,
      VARIABLE_VALUE,
      VARIABLE_PRINT,
      VARIABLE_NAME,
      VARIABLE_EDITABLE,
      VARIABLE_UPDATE
    };
  static const char *commands[] =
    {
      "delete",
      "numChildren",
      "children",
      "format",
      "type",
      "value",
      "print",
      "name",
      "editable",
      "update",
      NULL
    };
  struct varobj *var;
  char *varobj_name;
  int index, result;

  /* Get the current handle for this variable token (name). */
  varobj_name = Tcl_GetStringFromObj (objv[0], NULL);
  if (varobj_name == NULL)
    return TCL_ERROR;
  var = varobj_get_handle (varobj_name);


  if (objc < 2)
    {
      Tcl_WrongNumArgs (interp, 1, objv, "option ?arg...?");
      return TCL_ERROR;
    }

  if (Tcl_GetIndexFromObj (interp, objv[1], commands, "options", 0,
			   &index) != TCL_OK)
    return TCL_ERROR;

  result = TCL_OK;
  switch ((enum commands_enum) index)
    {
    case VARIABLE_DELETE:
      if (objc > 2)
	{
	  int len;
	  char *s = Tcl_GetStringFromObj (objv[2], &len);
	  if (*s == 'c' && strncmp (s, "children", len) == 0)
	    {
	      variable_delete (interp, var, 1 /* only children */ );
	      break;
	    }
	}
      variable_delete (interp, var, 0 /* var and children */ );
      break;

    case VARIABLE_NUM_CHILDREN:
      Tcl_SetObjResult (interp, Tcl_NewIntObj (varobj_get_num_children (var)));
      break;

    case VARIABLE_CHILDREN:
      {
	Tcl_Obj *children = variable_children (interp, var);
	Tcl_SetObjResult (interp, children);
      }
      break;

    case VARIABLE_FORMAT:
      result = variable_format (interp, objc, objv, var);
      break;

    case VARIABLE_TYPE:
      result = variable_type (interp, objc, objv, var);
      break;

    case VARIABLE_VALUE:
      result = variable_value (interp, objc, objv, var);
      break;

    case VARIABLE_PRINT:
      result = variable_print (interp, objc, objv, var);
      break;

    case VARIABLE_NAME:
      {
	std::string name = varobj_get_expression (var);
	Tcl_SetObjResult (interp, Tcl_NewStringObj (name.c_str (), -1));
      }
      break;

    case VARIABLE_EDITABLE:
      Tcl_SetObjResult (interp,
			Tcl_NewIntObj (varobj_get_attributes (var) & 0x00000001 /* Editable? */ ));
      break;

    case VARIABLE_UPDATE:
      /* Only root variables can be updated */
      {
	Tcl_Obj *obj = variable_update (interp, &var);
	Tcl_SetObjResult (interp, obj);
      }
      break;

    default:
      return TCL_ERROR;
    }

  return result;
}

/*
 * Variable object construction/destruction
 */

/* This function is responsible for processing the user's specifications
   and constructing a variable object. */
static int
variable_create (Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
  enum create_opts
    {
      CREATE_EXPR, CREATE_FRAME
    };
  static const char *create_options[] =
    {"-expr", "-frame", NULL};
  struct varobj *var;
  char *name;
  std::string obj_name;
  int index;
  CORE_ADDR frame = (CORE_ADDR) -1;
  enum varobj_type how_specified = USE_SELECTED_FRAME;

  /* REMINDER: This command may be invoked in the following ways:
     gdb_variable create [NAME] [-expr EXPR] [-frame FRAME]

     NAME  = name of object to create. If no NAME, then automatically create
     a name
     EXPR  = the gdb expression for which to create a variable. This will
     be the most common usage.
     FRAME = the address of the frame defining the variable's scope
  */
  name = NULL;
  if (objc)
    name = Tcl_GetStringFromObj (objv[0], NULL);
  if (name == NULL || *name == '-')
    {
      /* generate a name for this object */
      obj_name = varobj_gen_name ();
    }
  else
    {
      /* specified name for object */
      obj_name = name;
      objv++;
      objc--;
    }

  /* Run through all the possible options for this command */
  name = NULL;
  while (objc > 0)
    {
      if (Tcl_GetIndexFromObj (interp, objv[0], create_options, "options",
			       0, &index) != TCL_OK)
	{
	  result_ptr->flags |= GDBTK_IN_TCL_RESULT;
	  return TCL_ERROR;
	}

      switch ((enum create_opts) index)
	{
	case CREATE_EXPR:
	  name = Tcl_GetStringFromObj (objv[1], NULL);
	  objc--;
	  objv++;
	  break;

	case CREATE_FRAME:
	  {
	    char *str;
	    str = Tcl_GetStringFromObj (objv[1], NULL);
	    frame = string_to_core_addr (str);
	    how_specified = USE_SPECIFIED_FRAME;
	    objc--;
	    objv++;
	  }
	  break;

	default:
	  break;
	}

      objc--;
      objv++;
    }

  /* Create the variable */
  var = varobj_create (obj_name.c_str (), name, frame, how_specified);

  if (var != NULL)
    {
      /* Install a command into the interpreter that represents this
         object */
      install_variable (interp, obj_name.c_str ());
      Tcl_SetObjResult (interp, Tcl_NewStringObj (obj_name.c_str (), -1));
      result_ptr->flags |= GDBTK_IN_TCL_RESULT;

      return TCL_OK;
    }

  return TCL_ERROR;
}

/* Delete tcl representation of variable. */
static void
variable_delete_tcl (Tcl_Interp *interp, struct varobj *var,
                     int only_children_p)
{
  std::vector<varobj *>::iterator i;

  /* Delete children. */
  for (i = var->children.begin (); i != var->children.end (); i++)
    {
      varobj *child = *i;

      if (child)
        variable_delete_tcl (interp, child, 0);
    }

  if (only_children_p)
    return;

  /* Delete tcl variable now. */
  uninstall_variable (interp, var->obj_name.c_str ());
}

/* Delete the variable object VAR and its children */
/* If only_children_p, Delete only the children associated with the object. */
static void
variable_delete (Tcl_Interp *interp, struct varobj *var,
		 int only_children_p)
{
  variable_delete_tcl (interp, var, only_children_p);
  varobj_delete (var, only_children_p);
}

/* Return a list of all the children of VAR, creating them if necessary. */
static Tcl_Obj *
variable_children (Tcl_Interp *interp, struct varobj *var)
{
  Tcl_Obj *list = Tcl_NewListObj (0, NULL);
  int from = -1;
  int to = -1;
  const std::vector<varobj *> &children =
                                        varobj_list_children (var, &from, &to);
  const char *childname;
  int ix;

  if (from >= 0)
    {
      for (ix = from; ix < to; ++ix)
        {
          childname = varobj_get_objname (children[ix]);
          /* Add child to result list and install the Tcl command for it. */
          Tcl_ListObjAppendElement (NULL, list,
				    Tcl_NewStringObj (childname, -1));
          install_variable (interp, childname);
        }
    }

  return list;
}

/* Update the values for a variable and its children. */
/* NOTE:   Only root variables can be updated... */

static Tcl_Obj *
variable_update (Tcl_Interp *interp, struct varobj **var)
{
  Tcl_Obj *changed;
  std::vector<varobj_update_result> changes;
  std::vector<varobj_update_result>::iterator r;

  if (GDB_varobj_update (var, 1, changes) != GDB_OK)
    return Tcl_NewStringObj ("-1", -1);

  changed = Tcl_NewListObj (0, NULL);
  for (r = changes.begin (); r != changes.end (); r++)
    {
      switch (r->status)
	{
	case VAROBJ_IN_SCOPE:
	  {
	    Tcl_Obj *v = Tcl_NewStringObj (varobj_get_objname (r->varobj), -1);
	    Tcl_ListObjAppendElement (NULL, changed, v);
	  }
	  break;

	case VAROBJ_NOT_IN_SCOPE:
	case VAROBJ_INVALID:
	  /* These need to be (re-)implemented in the UI */
	  break;
	}
    }

  return changed;
}

/* This implements the format object command allowing
   the querying or setting of the object's display format. */
static int
variable_format (Tcl_Interp *interp, int objc,
		 Tcl_Obj *CONST objv[], struct varobj *var)
{
  if (objc > 2)
    {
      /* Set the format of VAR to given format */
      int len;
      char *fmt = Tcl_GetStringFromObj (objv[2], &len);
      if (strncmp (fmt, "natural", len) == 0)
	varobj_set_display_format (var, FORMAT_NATURAL);
      else if (strncmp (fmt, "binary", len) == 0)
	varobj_set_display_format (var, FORMAT_BINARY);
      else if (strncmp (fmt, "decimal", len) == 0)
	varobj_set_display_format (var, FORMAT_DECIMAL);
      else if (strncmp (fmt, "hexadecimal", len) == 0)
	varobj_set_display_format (var, FORMAT_HEXADECIMAL);
      else if (strncmp (fmt, "octal", len) == 0)
	varobj_set_display_format (var, FORMAT_OCTAL);
      else
	{
	  gdbtk_set_result (interp, "unknown display format \"",
			    fmt, "\": must be: \"natural\", \"binary\""
			    ", \"decimal\", \"hexadecimal\", or \"octal\"");
	  return TCL_ERROR;
	}
    }
  else
    {
      /* Report the current format */
      Tcl_Obj *fmt;

      /* FIXME: Use varobj_format_string[] instead */
      fmt = Tcl_NewStringObj (
			      format_string[(int) varobj_get_display_format (var)], -1);
      Tcl_SetObjResult (interp, fmt);
    }

  return TCL_OK;
}

/* This function implements the type object command, which returns the type of a
   variable in the interpreter (or an error). */
static int
variable_type (Tcl_Interp *interp, int objc,
	       Tcl_Obj *CONST objv[], struct varobj *var)
{
  size_t pos;
  std::string string;

  /* For the "fake" variables, do not return a type.
     Their type is NULL anyway */
  /* FIXME: varobj_get_type() calls type_print(), so we may have to wrap
     its call here and return TCL_ERROR in the case it errors out */
  string = varobj_get_type (var);
  if (string.length () == 0)
    {
      Tcl_ResetResult (interp);
      return TCL_OK;
    }

  /* gdb will print things out like "struct {...}" for anonymous structs.
     In gui-land, we don't want the {...}, so we strip it here. */
  pos = string.find ("{...}");
  if (pos != std::string::npos)
    {
      /* We have an anonymous struct/union/class/enum */
      if (pos && string[pos - 1] == ' ')
	pos--;
      string = string.substr (0, pos);
    }

  Tcl_SetObjResult (interp, Tcl_NewStringObj (string.c_str (), -1));
  return TCL_OK;
}

/* This function implements the value object command, which allows an object's
   value to be queried or set. */
static int
variable_value (Tcl_Interp *interp, int objc,
		Tcl_Obj *CONST objv[], struct varobj *var)
{
  std::string r;

  /* If we're setting the value of the variable, objv[2] will contain the
     variable's new value. */
  if (objc > 2)
    {
      /* FIXME: Do we need to test if val->error is set here?
         If so, make it an attribute. */
      if (varobj_get_attributes (var) & 0x00000001 /* Editable? */ )
	{
	  char *s;
	  int ok = 0;

	  s = Tcl_GetStringFromObj (objv[2], NULL);
	  try
	    {
	      ok = varobj_set_value (var, s);
	    }
          catch (const gdb_exception_error &)
	    {
	      ok = 0;
	    }

	  if (!ok)
	    {
	      gdbtk_set_result (interp, "Could not assign expression to variable object");
	      return TCL_ERROR;
	    }
	}

      Tcl_ResetResult (interp);
      return TCL_OK;
    }

  r = varobj_get_value (var);

  if (r.length () == 0)
    {
      gdbtk_set_result (interp, "Could not read variable object value after assignment");
      return TCL_ERROR;
    }
  else
    {
      Tcl_SetObjResult (interp, Tcl_NewStringObj (r.c_str (), -1));
      return TCL_OK;
    }
}

/* This function implements the print object command, which allows an object's
   value to be formatted for printing. */
static int
variable_print (Tcl_Interp *interp, int objc,
		Tcl_Obj *CONST objv[], struct varobj *var)
{
  string_file stream;
  int ret = TCL_ERROR;

  try
    {
      struct value_print_options opts;

      varobj_formatted_print_options (&opts, varobj_get_display_format (var));
      opts.deref_ref = 1;
      opts.raw = 0;
      common_val_print (var->value.get (), &stream, 0, &opts, current_language);
      Tcl_SetObjResult (interp, Tcl_NewStringObj (stream.data (), -1));
      ret = TCL_OK;
    }
  catch (const gdb_exception_error &except)
    {
      gdbtk_set_result (interp, "<error reading variable: %s>", except.message);
    }

  return ret;
}

/* Helper functions for the above */

/* Install the given variable VAR into the tcl interpreter with
   the object name NAME. */
static void
install_variable (Tcl_Interp *interp, const char *name)
{
  Tcl_CreateObjCommand (interp, name, variable_obj_command,
			NULL, NULL);
}

/* Uninstall the object VAR in the tcl interpreter. */
static void
uninstall_variable (Tcl_Interp *interp, const char *varname)
{
  Tcl_DeleteCommand (interp, varname);
}
