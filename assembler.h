/*
 * assembler.h
 *
 * This header declares the public interface of the assembler module.
 * The assembler module performs the two‑pass assembly of a single
 * preprocessed source file into machine code and produces the
 * corresponding output files (.ob, .ent, .ext).  It relies on the
 * symbol table and macro table modules for symbol management and
 * macro expansion.  Errors encountered during assembly are reported
 * via the standard output and the module signals whether code
 * generation should proceed.
 */

#ifndef ASSEMBLER_H
#define ASSEMBLER_H

#include <stddef.h>

#include "preprocessor.h"
#include "symbol_table.h"
#include "macro_table.h"

/* Assemble the preprocessed lines of a source file.  `basename`
 * contains the file name without extension and is used to derive the
 * output file names (e.g. basename.ob, basename.ent, basename.ext).
 * `lines` is an array of preprocessed lines (from the preprocessor)
 * with `count` elements.  The function returns 0 on successful
 * assembly and code generation or a non‑zero value if any errors
 * occurred.  In case of errors no output files are generated. */
int assemble_file(const char *basename, PreprocessedLine *lines, size_t count);

/* Convert a 10‑bit value into the "unique" base‑4 representation
 * required by the object file.  The value is treated as an unsigned
 * 10‑bit number (only the least significant 10 bits are used).  The
 * result is a NUL‑terminated string containing exactly 5 characters,
 * each in the range 'a'..'d' mapping to binary digits 00..11.  The
 * caller must provide a buffer of at least 6 bytes. */
void to_unique_base4(unsigned int value, char out[6]);

/* Convert an integer address (0‑255) into the "unique" base‑4
 * representation used for addresses in the object file.  The result
 * is a string of at most 4 characters (without leading zeros beyond
 * one).  The caller must supply a buffer with sufficient space (at
 * least 5 bytes). */
void address_to_unique_base4(unsigned int address, char *out);

#endif /* ASSEMBLER_H */
