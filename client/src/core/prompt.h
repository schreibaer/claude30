/* Prompt encoder: turns the text typed in the input area into the lines the
   bridge expects. Every line but the last ends in a backslash so the bridge
   joins them into one prompt; each line ends with CR. */
#ifndef PROMPT_H
#define PROMPT_H

/* Encodes text (line breaks as CR, LF or CR LF) into out.
   Returns the number of bytes written, or -1 if out is too small.
   *nlines receives the number of lines sent, so the caller knows how many
   continuation ready markers the bridge will send back (nlines - 1). */
long prompt_encode(const char *text, long len, char *out, long outcap, int *nlines);

/* Upper bound on the encoded size of len bytes of text. */
long prompt_encoded_max(long len);

#endif
