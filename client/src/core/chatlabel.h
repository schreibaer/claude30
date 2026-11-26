/* Labels for chat tabs: "Chat", then "Chat 2", "Chat 3", ... the lowest one
   no open chat tab has. Portable C with no Toolbox calls, host-tested. */
#ifndef CHATLABEL_H
#define CHATLABEL_H

/* The name a chat is opened with on the bridge (PROTOCOL.md, Chats). */
#define CHAT_WIRE_NAME "*"

/* Writes the lowest free label into out (cut to max - 1 characters), given
   the labels of the chat tabs now open. */
void chat_label(char *out, short max, const char *const *used, short nused);

#endif
