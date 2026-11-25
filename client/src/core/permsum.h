/* The bold headline above a permission request: one line built from the
   request's header ("Edit src/parser.py") and from counts taken from the body
   the Mac received. It helps the person judge the size of a request at a
   glance; it never replaces the body. Where it cannot be exact it says so.
   Portable C, host-tested; no Toolbox calls. */
#ifndef PERMSUM_H
#define PERMSUM_H

/* Width of text in the caller's font, in pixels. NULL: one unit per byte. */
typedef long (*PermsumMeasure)(const char *s, long n);

#define PERMSUM_MAX 160

/* Writes the headline to out (NUL-terminated, at most cap-1 bytes, never more
   than PERMSUM_MAX-1) and returns its length.
     header     the request's header, "<Tool> [(type)] <target>"
     body       all body lines back to back; line_ends[i] is the end offset of
                line i (the PermReq layout); nlines may be 0
     incomplete part of the request could not be kept: the headline then says
                to deny it, whatever the body holds
     measure, max_width   the line must be no wider than max_width; a target is
                cut with an ellipsis, a Bash command is never cut (a command
                that does not fit is described instead)
   Counts: Edit and MultiEdit bodies are unified diffs without the file
   headers, so every body line starting with '+' was added and every one
   starting with '-' was removed, even when its own text starts with "++" or
   "--"; lines starting with "@@" are hunks. A Write body is all '+' lines. */
long permsum_build(const char *header, long hlen,
                   const char *body, const long *line_ends, long nlines,
                   int incomplete,
                   PermsumMeasure measure, long max_width,
                   char *out, long cap);

#endif
