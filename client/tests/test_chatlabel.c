/* Labels for chat tabs: the lowest free "Chat", "Chat 2", ... */
#include "check.h"
#include "chatlabel.h"

static void label(char *out, short max, const char *const *used, short n)
{
    chat_label(out, max, used, n);
}

int main(void)
{
    char out[64];
    const char *used[6] = { 0 };

    label(out, sizeof(out), used, 0);
    CHECK(strcmp(out, "Chat") == 0);

    used[0] = "Chat";
    label(out, sizeof(out), used, 1);
    CHECK(strcmp(out, "Chat 2") == 0);

    used[1] = "Chat 2";
    used[2] = "Chat 3";
    label(out, sizeof(out), used, 3);
    CHECK(strcmp(out, "Chat 4") == 0);

    /* A closed chat's number is free again, the lowest first. */
    used[0] = "Chat 2";
    used[1] = "Chat 3";
    label(out, sizeof(out), used, 2);
    CHECK(strcmp(out, "Chat") == 0);
    used[0] = "Chat";
    used[1] = "Chat 3";
    label(out, sizeof(out), used, 2);
    CHECK(strcmp(out, "Chat 2") == 0);

    /* Six tabs could all be chats: the sixth label is "Chat 6". */
    used[0] = "Chat"; used[1] = "Chat 2"; used[2] = "Chat 3"; used[3] = "Chat 4"; used[4] = "Chat 5";
    label(out, sizeof(out), used, 5);
    CHECK(strcmp(out, "Chat 6") == 0);

    /* A small buffer cuts the label, never overruns. */
    used[0] = "Chat";
    memset(out, 'x', sizeof(out));
    label(out, 5, used, 1);
    CHECK(strcmp(out, "Chat") == 0);
    CHECK(out[5] == 'x');
    label(out, 1, used, 0);
    CHECK(out[0] == 0);

    CHECK_DONE();
}
