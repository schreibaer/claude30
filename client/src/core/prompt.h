/* Prompt encoder: turns the text typed in the input area into the lines the
   bridge expects. Each line starts with a prefix (the session's "<n>P "),
   every line but the last ends in a backslash so the bridge joins them into
   one prompt, and each line ends with CR. */
#ifndef PROMPT_H
#define PROMPT_H

/* Encodes text (line breaks as CR, LF or CR LF) into out.
   Returns the number of bytes written, or -1 if out is too small.
   *nlines receives the number of lines sent. */
long prompt_encode(const char *text, long len, const char *prefix,
                   char *out, long outcap, int *nlines);

/* Upper bound on the encoded size of len bytes of text. */
long prompt_encoded_max(long len, long prefix_len);

#endif
