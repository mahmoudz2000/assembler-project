/*
 * preprocessor.h
 *
 * The preprocessor reads an assembly source file, extracts macro
 * definitions and produces an expanded sequence of source lines with
 * all macro invocations replaced by their bodies.  The expansion
 * preserves the original line numbers to aid error reporting later
 * during the assembly passes.  The preprocessor relies on the macro
 * table defined in macro_table.h.
 */

#ifndef PREPROCESSOR_H
#define PREPROCESSOR_H

#include <stddef.h>

#include "macro_table.h"

/* Representation of a preprocessed line.  The `text` field holds the
 * actual line of assembly source.  The `orig_line` field stores the
 * line number in the original source file from which this line was
 * derived.  If the line comes from a macro expansion, `orig_line`
 * holds the line number of the macro invocation so that error
 * messages can reference the correct context.
 */
typedef struct {
    char *text;
    int orig_line;
} PreprocessedLine;

/* Preprocess a given source file.  On success the function allocates
 * an array of PreprocessedLine structures and stores it in
 * `*out_lines`.  The number of lines in the array is stored in
 * `*out_count`.  The caller is responsible for freeing the array and
 * the text fields of each element via `pp_free_lines()`.  If the
 * function encounters an unrecoverable error (e.g. file not found or
 * macro redefinition), it prints an error message and returns a
 * non‑zero value.  The macro table is populated with all macro
 * definitions encountered.
 */
int pp_process_file(const char *filename, MacroTable *macro_table,
                    PreprocessedLine **out_lines, size_t *out_count);

/* Free an array of PreprocessedLine structures.  Must be called to
 * release memory allocated by pp_process_file().  Passing NULL does
 * nothing.  The macro table is not affected.
 */
void pp_free_lines(PreprocessedLine *lines, size_t count);

#endif /* PREPROCESSOR_H */