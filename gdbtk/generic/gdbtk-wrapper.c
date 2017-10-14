/* longjmp-free interface between gdb and gdbtk.
   Copyright (C) 1999-2016 Free Software Foundation, Inc.

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
#include "value.h"
#include "varobj.h"
#include "block.h"
#include "exceptions.h"
#include "language.h"
#include "valprint.h"
#include "gdbtk-wrapper.h"

/*
 * Wrapper functions exported to the world
 */

gdb_result GDB_value_fetch_lazy (value_ptr);

gdb_result GDB_evaluate_expression (struct expression *, value_ptr *);

gdb_result GDB_type_print (value_ptr, char *, struct ui_file *, int);

gdb_result GDB_value_equal (value_ptr, value_ptr, int *);

gdb_result GDB_evaluate_type (struct expression *exp, value_ptr * result);

gdb_result GDB_block_for_pc (CORE_ADDR pc, const struct block **result);

gdb_result GDB_block_innermost_frame (struct block *block,
				      struct frame_info **result);

gdb_result GDB_reinit_frame_cache (void);

gdb_result GDB_value_ind (value_ptr val, value_ptr * rval);

gdb_result GDB_value_slice (value_ptr val, int low, int num,
			    value_ptr * rval);

gdb_result GDB_value_coerce_array (value_ptr val, value_ptr * rval);

gdb_result GDB_value_struct_elt (value_ptr * argp, value_ptr * args,
				 char *name, int *static_memfunc,
				 char *err, value_ptr * rval);

gdb_result GDB_value_cast (struct type *type, value_ptr val,
			   value_ptr * rval);

gdb_result GDB_get_frame_block (struct frame_info *fi,
				const struct block **rval);

gdb_result GDB_get_prev_frame (struct frame_info *fi,
			       struct frame_info **result);

gdb_result GDB_get_next_frame (struct frame_info *fi,
			       struct frame_info **result);

gdb_result GDB_find_relative_frame (struct frame_info *fi,
				    int *start, struct frame_info **result);

gdb_result GDB_get_current_frame (struct frame_info **result);

gdb_result GDB_varobj_update (struct varobj **varp, int xplicit,
			      VEC (varobj_update_result) **changes);

/* Error catcher macro. */
#define GDBTK_CATCH_ERRORS(statement)					\
		TRY							\
		  {							\
		    statement;						\
		  }							\
		CATCH (ex, RETURN_MASK_ERROR)				\
		  {							\
		    exception_print (gdb_stderr, ex);			\
		    return GDB_ERROR;					\
		  }							\
		END_CATCH						\
		return GDB_OK;


gdb_result
GDB_type_print (value_ptr val, char *varstring,
		struct ui_file *stream, int show)
{
  GDBTK_CATCH_ERRORS (type_print (value_type (val), varstring, stream, show));
}

gdb_result
GDB_value_fetch_lazy (value_ptr value)
{
  GDBTK_CATCH_ERRORS (value_fetch_lazy (value));
}

gdb_result
GDB_evaluate_expression (struct expression *exp, value_ptr *value)
{
  GDBTK_CATCH_ERRORS (*value = (value_ptr) evaluate_expression (exp));
}

gdb_result
GDB_value_equal (value_ptr val1, value_ptr val2, int *result)
{
  GDBTK_CATCH_ERRORS (*result = value_equal (val1, val2));
}

gdb_result
GDB_evaluate_type (struct expression *exp, value_ptr *result)
{
  GDBTK_CATCH_ERRORS (*result = (value_ptr) evaluate_type (exp));
}

gdb_result
GDB_block_for_pc (CORE_ADDR pc, const struct block **result)
{
  GDBTK_CATCH_ERRORS (*result = block_for_pc (pc));
}

gdb_result
GDB_block_innermost_frame (struct block *block, struct frame_info **result)
{
  GDBTK_CATCH_ERRORS (*result = block_innermost_frame (block));
}

gdb_result
GDB_reinit_frame_cache (void)
{
  GDBTK_CATCH_ERRORS (reinit_frame_cache ());
}

gdb_result
GDB_value_ind (value_ptr val, value_ptr *rval)
{
  GDBTK_CATCH_ERRORS (*rval = (value_ptr) value_ind (val));
}

gdb_result
GDB_value_slice (value_ptr val, int low, int num, value_ptr *rval)
{
  GDBTK_CATCH_ERRORS (*rval = (value_ptr) value_slice (val, low, num));
}

gdb_result
GDB_value_coerce_array (value_ptr val, value_ptr *rval)
{
  GDBTK_CATCH_ERRORS (*rval = (value_ptr) value_coerce_array (val));
}

gdb_result
GDB_value_struct_elt (value_ptr *argp,
		      value_ptr *args,
		      char *name,
		      int *static_memfunc,
		      char *err,
		      value_ptr *rval)
{
  GDBTK_CATCH_ERRORS (*rval = (value_ptr) value_struct_elt (argp, args, name,
                                                            static_memfunc,
                                                            err));
}

gdb_result
GDB_value_cast (struct type *type, value_ptr val, value_ptr *rval)
{
  GDBTK_CATCH_ERRORS (*rval = (value_ptr) value_cast (type, val));
}

gdb_result
GDB_get_frame_block (struct frame_info *fi, const struct block **rval)
{
  GDBTK_CATCH_ERRORS (*rval = get_frame_block (fi, NULL));
}

gdb_result
GDB_get_prev_frame (struct frame_info *fi, struct frame_info **result)
{
  GDBTK_CATCH_ERRORS (*result = get_prev_frame (fi));
}

gdb_result
GDB_get_next_frame (struct frame_info *fi, struct frame_info **result)
{
  GDBTK_CATCH_ERRORS (*result = get_next_frame (fi));
}

gdb_result
GDB_find_relative_frame (struct frame_info *fi, int *start,
			 struct frame_info **result)
{
  GDBTK_CATCH_ERRORS (*result = find_relative_frame (fi, start));
}

gdb_result
GDB_get_current_frame (struct frame_info **result)
{
  GDBTK_CATCH_ERRORS (*result = get_current_frame ());
}

gdb_result
GDB_varobj_update (struct varobj **varp, int xplicit,
		   VEC (varobj_update_result) **changes)
{
  GDBTK_CATCH_ERRORS (*changes = varobj_update (varp, xplicit));
}
