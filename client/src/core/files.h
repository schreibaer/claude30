/* File transfers over protocol version 2: folder listings, piece reads and
   block saves, per session, with CRC-32 checks and resends (PROTOCOL.md,
   "Files"). Portable C; the types live in protocol.h.

   Reads and saves run as transfers in a small table. Each one reports its
   outcome as an event (EV_PIECE, EV_READ_FAILED, EV_SAVED, ...) during which
   proto_xfer() returns it. A transfer is free again after its last event. */
#ifndef FILES_H
#define FILES_H

#include "protocol.h"

#define PIECE_LINES   200
#define PIECE_BYTES   4096
#define SAVE_BLOCK    2048
#define XFER_TRIES    3

/* What the bridge allows: FILES_OFF, FILES_RO or FILES_RW. */
int proto_files_mode(const Proto *p);

/* Lists a folder of session sid's project ("" is the project itself).
   Answers with EV_LIST_BEGIN, EV_LIST_ENTRY..., EV_LIST_END or EV_LIST_FAILED. */
int proto_list(Proto *p, int sid, const char *path);
/* Splits an EV_LIST_ENTRY text. Returns 0 if it is malformed. */
int files_parse_entry(const char *t, long n, int *is_dir, long *size,
                      const char **name, long *name_len);

/* Reads up to count lines from first (count 0 checks the version token
   only). Returns the transfer's slot, or -1 if none is free. */
int proto_read(Proto *p, int sid, long owner, const char *path,
               long first, long count, long budget);

/* Saves text (CR line breaks) over path, if the file still has the given
   version token. The text is copied. Returns the slot, or -1. */
int proto_save(Proto *p, int sid, long owner, const char *path,
               const char *token, const char *text, long len);
/* After EV_SAVE_CONFLICT: replace the file anyway. */
int proto_save_force(Proto *p, int slot);
/* Forgets a transfer without further events. */
void proto_xfer_cancel(Proto *p, int slot);
/* Sends the next block of each save that is due. Call once per pass, so
   typing stays responsive between blocks. */
void proto_files_step(Proto *p);

/* The transfer the current event is about, or NULL. */
const FileXfer *proto_xfer(const Proto *p);
int proto_xfer_slot(const Proto *p);

/* Internal, for protocol.c. */
void files_line(Proto *p, int sid, const char *tag, long tlen, const char *pay, long plen);
void files_tick(Proto *p);
/* Fails every transfer and listing of sid (0: of every session). */
void files_fail(Proto *p, int sid, const char *why);

#endif
