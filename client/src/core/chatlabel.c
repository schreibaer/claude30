#include "chatlabel.h"
#include <string.h>

static void make(char *s, short n)
{
    /* "Chat" for 1, else "Chat <n>" (n is small: at most six tabs). */
    strcpy(s, "Chat");
    if (n > 1) {
        short len = 4;
        s[len++] = ' ';
        if (n >= 10)
            s[len++] = (char)('0' + n / 10);
        s[len++] = (char)('0' + n % 10);
        s[len] = 0;
    }
}

void chat_label(char *out, short max, const char *const *used, short nused)
{
    char want[16];
    short n, i;
    if (max <= 0)
        return;
    for (n = 1;; n++) {
        make(want, n);
        for (i = 0; i < nused; i++)
            if (strcmp(used[i], want) == 0)
                break;
        if (i == nused || n > 99)
            break;
    }
    strncpy(out, want, (size_t)max - 1);
    out[max - 1] = 0;
}
