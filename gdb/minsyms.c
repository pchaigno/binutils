/* GDB routines for manipulating the minimal symbol tables.
   Copyright 1992, 1993, 1994, 1995 Free Software Foundation, Inc.
   Contributed by Cygnus Support, using pieces from other GDB modules.

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
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.  */


/* This file contains support routines for creating, manipulating, and
   destroying minimal symbol tables.

   Minimal symbol tables are used to hold some very basic information about
   all defined global symbols (text, data, bss, abs, etc).  The only two
   required pieces of information are the symbol's name and the address
   associated with that symbol.

   In many cases, even if a file was compiled with no special options for
   debugging at all, as long as was not stripped it will contain sufficient
   information to build useful minimal symbol tables using this structure.
   
   Even when a file contains enough debugging information to build a full
   symbol table, these minimal symbols are still useful for quickly mapping
   between names and addresses, and vice versa.  They are also sometimes used
   to figure out what full symbol table entries need to be read in. */


#include "defs.h"
#include <string.h>
#include "symtab.h"
#include "bfd.h"
#include "symfile.h"
#include "objfiles.h"
#include "demangle.h"
#include "gdb-stabs.h"

/* Accumulate the minimal symbols for each objfile in bunches of BUNCH_SIZE.
   At the end, copy them all into one newly allocated location on an objfile's
   symbol obstack.  */

#define BUNCH_SIZE 127

struct msym_bunch
{
  struct msym_bunch *next;
  struct minimal_symbol contents[BUNCH_SIZE];
};

/* Bunch currently being filled up.
   The next field points to chain of filled bunches.  */

static struct msym_bunch *msym_bunch;

/* Number of slots filled in current bunch.  */

static int msym_bunch_index;

/* Total number of minimal symbols recorded so far for the objfile.  */

static int msym_count;

/* Prototypes for local functions. */

static int
compare_minimal_symbols PARAMS ((const void *, const void *));

static int
compact_minimal_symbols PARAMS ((struct minimal_symbol *, int));

/* Look through all the current minimal symbol tables and find the
   first minimal symbol that matches NAME.  If OBJF is non-NULL, limit
   the search to that objfile.  If SFILE is non-NULL, limit the search
   to that source file.  Returns a pointer to the minimal symbol that
   matches, or NULL if no match is found.

   Note:  One instance where there may be duplicate minimal symbols with
   the same name is when the symbol tables for a shared library and the
   symbol tables for an executable contain global symbols with the same
   names (the dynamic linker deals with the duplication). */

struct minimal_symbol *
lookup_minimal_symbol (name, sfile, objf)
     register const char *name;
     const char *sfile;
     struct objfile *objf;
{
  struct objfile *objfile;
  struct minimal_symbol *msymbol;
  struct minimal_symbol *found_symbol = NULL;
  struct minimal_symbol *found_file_symbol = NULL;
  struct minimal_symbol *trampoline_symbol = NULL;

#ifdef SOFUN_ADDRESS_MAYBE_MISSING
  if (sfile != NULL)
    {
      char *p = strrchr (sfile, '/');
      if (p != NULL)
	sfile = p + 1;
    }
#endif

  for (objfile = object_files;
       objfile != NULL && found_symbol == NULL;
       objfile = objfile -> next)
    {
      if (objf == NULL || objf == objfile)
	{
	  for (msymbol = objfile -> msymbols;
	       msymbol != NULL && SYMBOL_NAME (msymbol) != NULL &&
	       found_symbol == NULL;
	       msymbol++)
	    {
	      if (SYMBOL_MATCHES_NAME (msymbol, name))
		{
		  switch (MSYMBOL_TYPE (msymbol))
		    {
		    case mst_file_text:
		    case mst_file_data:
		    case mst_file_bss:
#ifdef SOFUN_ADDRESS_MAYBE_MISSING
		      if (sfile == NULL || STREQ (msymbol->filename, sfile))
			found_file_symbol = msymbol;
#else
		      /* We have neither the ability nor the need to
			 deal with the SFILE parameter.  If we find
			 more than one symbol, just return the latest
			 one (the user can't expect useful behavior in
			 that case).  */
		      found_file_symbol = msymbol;
#endif
		      break;

		    case mst_solib_trampoline:

		      /* If a trampoline symbol is found, we prefer to
			 keep looking for the *real* symbol. If the
			 actual symbol is not found, then we'll use the
			 trampoline entry. */
		      if (trampoline_symbol == NULL)
			trampoline_symbol = msymbol;
		      break;

		    case mst_unknown:
		    default:
		      found_symbol = msymbol;
		      break;
		    }
		}
	    }
	}
    }
  /* External symbols are best.  */
  if (found_symbol)
    return found_symbol;

  /* File-local symbols are next best.  */
  if (found_file_symbol)
    return found_file_symbol;

  /* Symbols for shared library trampolines are next best.  */
  if (trampoline_symbol)
    return trampoline_symbol;

  return NULL;
}


/* Search through the minimal symbol table for each objfile and find the
   symbol whose address is the largest address that is still less than or
   equal to PC.  Returns a pointer to the minimal symbol if such a symbol
   is found, or NULL if PC is not in a suitable range.  Note that we need
   to look through ALL the minimal symbol tables before deciding on the
   symbol that comes closest to the specified PC.  This is because objfiles
   can overlap, for example objfile A has .text at 0x100 and .data at 0x40000
   and objfile B has .text at 0x234 and .data at 0x40048.  */

struct minimal_symbol *
lookup_minimal_symbol_by_pc (pc)
     register CORE_ADDR pc;
{
  register int lo;
  register int hi;
  register int new;
  register struct objfile *objfile;
  register struct minimal_symbol *msymbol;
  register struct minimal_symbol *best_symbol = NULL;

  for (objfile = object_files;
       objfile != NULL;
       objfile = objfile -> next)
    {
      /* If this objfile has a minimal symbol table, go search it using
	 a binary search.  Note that a minimal symbol table always consists
	 of at least two symbols, a "real" symbol and the terminating
	 "null symbol".  If there are no real symbols, then there is no
	 minimal symbol table at all. */

      if ((msymbol = objfile -> msymbols) != NULL)
	{
	  lo = 0;
	  hi = objfile -> minimal_symbol_count - 1;

	  /* This code assumes that the minimal symbols are sorted by
	     ascending address values.  If the pc value is greater than or
	     equal to the first symbol's address, then some symbol in this
	     minimal symbol table is a suitable candidate for being the
	     "best" symbol.  This includes the last real symbol, for cases
	     where the pc value is larger than any address in this vector.

	     By iterating until the address associated with the current
	     hi index (the endpoint of the test interval) is less than
	     or equal to the desired pc value, we accomplish two things:
	     (1) the case where the pc value is larger than any minimal
	     symbol address is trivially solved, (2) the address associated
	     with the hi index is always the one we want when the interation
	     terminates.  In essence, we are iterating the test interval
	     down until the pc value is pushed out of it from the high end.

	     Warning: this code is trickier than it would appear at first. */

	  /* Should also requires that pc is <= end of objfile.  FIXME! */
	  if (pc >= SYMBOL_VALUE_ADDRESS (&msymbol[lo]))
	    {
	      while (SYMBOL_VALUE_ADDRESS (&msymbol[hi]) > pc)
		{
		  /* pc is still strictly less than highest address */
		  /* Note "new" will always be >= lo */
		  new = (lo + hi) / 2;
		  if ((SYMBOL_VALUE_ADDRESS (&msymbol[new]) >= pc) ||
		      (lo == new))
		    {
		      hi = new;
		    }
		  else
		    {
		      lo = new;
		    }
		}
	      /* The minimal symbol indexed by hi now is the best one in this
		 objfile's minimal symbol table.  See if it is the best one
		 overall. */

	      /* Skip any absolute symbols.  This is apparently what adb
		 and dbx do, and is needed for the CM-5.  There are two
		 known possible problems: (1) on ELF, apparently end, edata,
		 etc. are absolute.  Not sure ignoring them here is a big
		 deal, but if we want to use them, the fix would go in
		 elfread.c.  (2) I think shared library entry points on the
		 NeXT are absolute.  If we want special handling for this
		 it probably should be triggered by a special
		 mst_abs_or_lib or some such.  */
	      while (hi >= 0
		     && msymbol[hi].type == mst_abs)
		--hi;

	      if (hi >= 0
		  && ((best_symbol == NULL) ||
		      (SYMBOL_VALUE_ADDRESS (best_symbol) < 
		       SYMBOL_VALUE_ADDRESS (&msymbol[hi]))))
		{
		  best_symbol = &msymbol[hi];
		}
	    }
	}
    }
  return (best_symbol);
}

#ifdef SOFUN_ADDRESS_MAYBE_MISSING
CORE_ADDR
find_stab_function_addr (namestring, pst, objfile)
     char *namestring;
     struct partial_symtab *pst;
     struct objfile *objfile;
{
  struct minimal_symbol *msym;
  char *p;
  int n;

  p = strchr (namestring, ':');
  if (p == NULL)
    p = namestring;
  n = p - namestring;
  p = alloca (n + 1);
  strncpy (p, namestring, n);
  p[n] = 0;

  msym = lookup_minimal_symbol (p, pst->filename, objfile);
  return msym == NULL ? 0 : SYMBOL_VALUE_ADDRESS (msym);
}
#endif /* SOFUN_ADDRESS_MAYBE_MISSING */


/* Return leading symbol character for a BFD. If BFD is NULL,
   return the leading symbol character from the main objfile.  */

static int get_symbol_leading_char PARAMS ((bfd *));

static int
get_symbol_leading_char (abfd)
     bfd * abfd;
{
  if (abfd != NULL)
    return bfd_get_symbol_leading_char (abfd);
  if (symfile_objfile != NULL && symfile_objfile->obfd != NULL)
    return bfd_get_symbol_leading_char (symfile_objfile->obfd);
  return 0;
}

/* Prepare to start collecting minimal symbols.  Note that presetting
   msym_bunch_index to BUNCH_SIZE causes the first call to save a minimal
   symbol to allocate the memory for the first bunch. */

void
init_minimal_symbol_collection ()
{
  msym_count = 0;
  msym_bunch = NULL;
  msym_bunch_index = BUNCH_SIZE;
}

void
prim_record_minimal_symbol (name, address, ms_type, objfile)
     const char *name;
     CORE_ADDR address;
     enum minimal_symbol_type ms_type;
     struct objfile *objfile;
{
  int section;

  switch (ms_type)
    {
    case mst_text:
    case mst_file_text:
    case mst_solib_trampoline:
      section = SECT_OFF_TEXT;
      break;
    case mst_data:
    case mst_file_data:
      section = SECT_OFF_DATA;
      break;
    case mst_bss:
    case mst_file_bss:
      section = SECT_OFF_BSS;
      break;
    default:
      section = -1;
    }

  prim_record_minimal_symbol_and_info (name, address, ms_type,
				       NULL, section, objfile);
}

/* Record a minimal symbol in the msym bunches.  Returns the symbol
   newly created.  */
struct minimal_symbol *
prim_record_minimal_symbol_and_info (name, address, ms_type, info, section,
				     objfile)
     const char *name;
     CORE_ADDR address;
     enum minimal_symbol_type ms_type;
     char *info;
     int section;
     struct objfile *objfile;
{
  register struct msym_bunch *new;
  register struct minimal_symbol *msymbol;

  if (ms_type == mst_file_text)
    {
      /* Don't put gcc_compiled, __gnu_compiled_cplus, and friends into
	 the minimal symbols, because if there is also another symbol
	 at the same address (e.g. the first function of the file),
	 lookup_minimal_symbol_by_pc would have no way of getting the
	 right one.  */
      if (name[0] == 'g'
	  && (strcmp (name, GCC_COMPILED_FLAG_SYMBOL) == 0
	      || strcmp (name, GCC2_COMPILED_FLAG_SYMBOL) == 0))
	return;

      {
	const char *tempstring = name;
	if (tempstring[0] == get_symbol_leading_char (objfile->obfd))
	  ++tempstring;
	if (STREQN (tempstring, "__gnu_compiled", 14))
	  return;
      }
    }

  if (msym_bunch_index == BUNCH_SIZE)
    {
      new = (struct msym_bunch *) xmalloc (sizeof (struct msym_bunch));
      msym_bunch_index = 0;
      new -> next = msym_bunch;
      msym_bunch = new;
    }
  msymbol = &msym_bunch -> contents[msym_bunch_index];
  SYMBOL_NAME (msymbol) = (char *) name;
  SYMBOL_INIT_LANGUAGE_SPECIFIC (msymbol, language_unknown);
  SYMBOL_VALUE_ADDRESS (msymbol) = address;
  SYMBOL_SECTION (msymbol) = section;

  MSYMBOL_TYPE (msymbol) = ms_type;
  /* FIXME:  This info, if it remains, needs its own field.  */
  MSYMBOL_INFO (msymbol) = info; /* FIXME! */
  msym_bunch_index++;
  msym_count++;
  return msymbol;
}

/* Compare two minimal symbols by address and return a signed result based
   on unsigned comparisons, so that we sort into unsigned numeric order.  */

static int
compare_minimal_symbols (fn1p, fn2p)
     const PTR fn1p;
     const PTR fn2p;
{
  register const struct minimal_symbol *fn1;
  register const struct minimal_symbol *fn2;

  fn1 = (const struct minimal_symbol *) fn1p;
  fn2 = (const struct minimal_symbol *) fn2p;

  if (SYMBOL_VALUE_ADDRESS (fn1) < SYMBOL_VALUE_ADDRESS (fn2))
    {
      return (-1);
    }
  else if (SYMBOL_VALUE_ADDRESS (fn1) > SYMBOL_VALUE_ADDRESS (fn2))
    {
      return (1);
    }
  else
    {
      return (0);
    }
}

/* Discard the currently collected minimal symbols, if any.  If we wish
   to save them for later use, we must have already copied them somewhere
   else before calling this function.

   FIXME:  We could allocate the minimal symbol bunches on their own
   obstack and then simply blow the obstack away when we are done with
   it.  Is it worth the extra trouble though? */

/* ARGSUSED */
void
discard_minimal_symbols (foo)
     int foo;
{
  register struct msym_bunch *next;

  while (msym_bunch != NULL)
    {
      next = msym_bunch -> next;
      free ((PTR)msym_bunch);
      msym_bunch = next;
    }
}

/* Compact duplicate entries out of a minimal symbol table by walking
   through the table and compacting out entries with duplicate addresses
   and matching names.  Return the number of entries remaining.

   On entry, the table resides between msymbol[0] and msymbol[mcount].
   On exit, it resides between msymbol[0] and msymbol[result_count].

   When files contain multiple sources of symbol information, it is
   possible for the minimal symbol table to contain many duplicate entries.
   As an example, SVR4 systems use ELF formatted object files, which
   usually contain at least two different types of symbol tables (a
   standard ELF one and a smaller dynamic linking table), as well as
   DWARF debugging information for files compiled with -g.

   Without compacting, the minimal symbol table for gdb itself contains
   over a 1000 duplicates, about a third of the total table size.  Aside
   from the potential trap of not noticing that two successive entries
   identify the same location, this duplication impacts the time required
   to linearly scan the table, which is done in a number of places.  So we
   just do one linear scan here and toss out the duplicates.

   Note that we are not concerned here about recovering the space that
   is potentially freed up, because the strings themselves are allocated
   on the symbol_obstack, and will get automatically freed when the symbol
   table is freed.  The caller can free up the unused minimal symbols at
   the end of the compacted region if their allocation strategy allows it.

   Also note we only go up to the next to last entry within the loop
   and then copy the last entry explicitly after the loop terminates.

   Since the different sources of information for each symbol may
   have different levels of "completeness", we may have duplicates
   that have one entry with type "mst_unknown" and the other with a
   known type.  So if the one we are leaving alone has type mst_unknown,
   overwrite its type with the type from the one we are compacting out.  */

static int
compact_minimal_symbols (msymbol, mcount)
     struct minimal_symbol *msymbol;
     int mcount;
{
  struct minimal_symbol *copyfrom;
  struct minimal_symbol *copyto;

  if (mcount > 0)
    {
      copyfrom = copyto = msymbol;
      while (copyfrom < msymbol + mcount - 1)
	{
	  if (SYMBOL_VALUE_ADDRESS (copyfrom) == 
	      SYMBOL_VALUE_ADDRESS ((copyfrom + 1)) &&
	      (STREQ (SYMBOL_NAME (copyfrom), SYMBOL_NAME ((copyfrom + 1)))))
	    {
	      if (MSYMBOL_TYPE((copyfrom + 1)) == mst_unknown)
		{
		  MSYMBOL_TYPE ((copyfrom + 1)) = MSYMBOL_TYPE (copyfrom);
		}
	      copyfrom++;
	    }
	  else
	    {
	      *copyto++ = *copyfrom++;
	    }
	}
      *copyto++ = *copyfrom++;
      mcount = copyto - msymbol;
    }
  return (mcount);
}

/* Add the minimal symbols in the existing bunches to the objfile's official
   minimal symbol table.  In most cases there is no minimal symbol table yet
   for this objfile, and the existing bunches are used to create one.  Once
   in a while (for shared libraries for example), we add symbols (e.g. common
   symbols) to an existing objfile.

   Because of the way minimal symbols are collected, we generally have no way
   of knowing what source language applies to any particular minimal symbol.
   Specifically, we have no way of knowing if the minimal symbol comes from a
   C++ compilation unit or not.  So for the sake of supporting cached
   demangled C++ names, we have no choice but to try and demangle each new one
   that comes in.  If the demangling succeeds, then we assume it is a C++
   symbol and set the symbol's language and demangled name fields
   appropriately.  Note that in order to avoid unnecessary demanglings, and
   allocating obstack space that subsequently can't be freed for the demangled
   names, we mark all newly added symbols with language_auto.  After
   compaction of the minimal symbols, we go back and scan the entire minimal
   symbol table looking for these new symbols.  For each new symbol we attempt
   to demangle it, and if successful, record it as a language_cplus symbol
   and cache the demangled form on the symbol obstack.  Symbols which don't
   demangle are marked as language_unknown symbols, which inhibits future
   attempts to demangle them if we later add more minimal symbols. */

void
install_minimal_symbols (objfile)
     struct objfile *objfile;
{
  register int bindex;
  register int mcount;
  register struct msym_bunch *bunch;
  register struct minimal_symbol *msymbols;
  int alloc_count;
  register char leading_char;

  if (msym_count > 0)
    {
      /* Allocate enough space in the obstack, into which we will gather the
	 bunches of new and existing minimal symbols, sort them, and then
	 compact out the duplicate entries.  Once we have a final table,
	 we will give back the excess space.  */

      alloc_count = msym_count + objfile->minimal_symbol_count + 1;
      obstack_blank (&objfile->symbol_obstack,
		     alloc_count * sizeof (struct minimal_symbol));
      msymbols = (struct minimal_symbol *)
		 obstack_base (&objfile->symbol_obstack);

      /* Copy in the existing minimal symbols, if there are any.  */

      if (objfile->minimal_symbol_count)
        memcpy ((char *)msymbols, (char *)objfile->msymbols, 
		objfile->minimal_symbol_count * sizeof (struct minimal_symbol));

      /* Walk through the list of minimal symbol bunches, adding each symbol
	 to the new contiguous array of symbols.  Note that we start with the
	 current, possibly partially filled bunch (thus we use the current
	 msym_bunch_index for the first bunch we copy over), and thereafter
	 each bunch is full. */
      
      mcount = objfile->minimal_symbol_count;
      leading_char = get_symbol_leading_char (objfile->obfd);
      
      for (bunch = msym_bunch; bunch != NULL; bunch = bunch -> next)
	{
	  for (bindex = 0; bindex < msym_bunch_index; bindex++, mcount++)
	    {
	      msymbols[mcount] = bunch -> contents[bindex];
	      SYMBOL_LANGUAGE (&msymbols[mcount]) = language_auto;
	      if (SYMBOL_NAME (&msymbols[mcount])[0] == leading_char)
		{
		  SYMBOL_NAME(&msymbols[mcount])++;
		}
	    }
	  msym_bunch_index = BUNCH_SIZE;
	}

      /* Sort the minimal symbols by address.  */
      
      qsort (msymbols, mcount, sizeof (struct minimal_symbol),
	     compare_minimal_symbols);
      
      /* Compact out any duplicates, and free up whatever space we are
	 no longer using.  */
      
      mcount = compact_minimal_symbols (msymbols, mcount);

      obstack_blank (&objfile->symbol_obstack,
	(mcount + 1 - alloc_count) * sizeof (struct minimal_symbol));
      msymbols = (struct minimal_symbol *)
	obstack_finish (&objfile->symbol_obstack);

      /* We also terminate the minimal symbol table with a "null symbol",
	 which is *not* included in the size of the table.  This makes it
	 easier to find the end of the table when we are handed a pointer
	 to some symbol in the middle of it.  Zero out the fields in the
	 "null symbol" allocated at the end of the array.  Note that the
	 symbol count does *not* include this null symbol, which is why it
	 is indexed by mcount and not mcount-1. */

      SYMBOL_NAME (&msymbols[mcount]) = NULL;
      SYMBOL_VALUE_ADDRESS (&msymbols[mcount]) = 0;
      MSYMBOL_INFO (&msymbols[mcount]) = NULL;
      MSYMBOL_TYPE (&msymbols[mcount]) = mst_unknown;
      SYMBOL_INIT_LANGUAGE_SPECIFIC (&msymbols[mcount], language_unknown);

      /* Attach the minimal symbol table to the specified objfile.
	 The strings themselves are also located in the symbol_obstack
	 of this objfile.  */

      objfile -> minimal_symbol_count = mcount;
      objfile -> msymbols = msymbols;

      /* Now walk through all the minimal symbols, selecting the newly added
	 ones and attempting to cache their C++ demangled names. */

      for ( ; mcount-- > 0 ; msymbols++)
	{
	  SYMBOL_INIT_DEMANGLED_NAME (msymbols, &objfile->symbol_obstack);
	}
    }
}

/* Sort all the minimal symbols in OBJFILE.  */

void
msymbols_sort (objfile)
     struct objfile *objfile;
{
  qsort (objfile->msymbols, objfile->minimal_symbol_count,
	 sizeof (struct minimal_symbol), compare_minimal_symbols);
}

/* Check if PC is in a shared library trampoline code stub.
   Return minimal symbol for the trampoline entry or NULL if PC is not
   in a trampoline code stub.  */

struct minimal_symbol *
lookup_solib_trampoline_symbol_by_pc (pc)
     CORE_ADDR pc;
{
  struct minimal_symbol *msymbol = lookup_minimal_symbol_by_pc (pc);

  if (msymbol != NULL && MSYMBOL_TYPE (msymbol) == mst_solib_trampoline)
    return msymbol;
  return NULL;
}

/* If PC is in a shared library trampoline code stub, return the
   address of the `real' function belonging to the stub.
   Return 0 if PC is not in a trampoline code stub or if the real
   function is not found in the minimal symbol table.

   We may fail to find the right function if a function with the
   same name is defined in more than one shared library, but this
   is considered bad programming style. We could return 0 if we find
   a duplicate function in case this matters someday.  */

CORE_ADDR
find_solib_trampoline_target (pc)
     CORE_ADDR pc;
{
  struct objfile *objfile;
  struct minimal_symbol *msymbol;
  struct minimal_symbol *tsymbol = lookup_solib_trampoline_symbol_by_pc (pc);

  if (tsymbol != NULL)
    {
      ALL_MSYMBOLS (objfile, msymbol)
	{
	  if (MSYMBOL_TYPE (msymbol) == mst_text
	      && STREQ (SYMBOL_NAME (msymbol), SYMBOL_NAME (tsymbol)))
	    return SYMBOL_VALUE_ADDRESS (msymbol);
	}
    }
  return 0;
}

