#include "promptmarks.h"
#include <string.h>

void pm_init(PromptMarks *m)
{
    m->n = 0;
}

void pm_add(PromptMarks *m, long off)
{
    if (m->n > 0 && off <= m->off[m->n - 1])
        return;                   /* not after the last one: ignore */
    if (m->n == PM_MAX) {
        memmove(m->off, m->off + 1, (PM_MAX - 1) * sizeof(long));
        m->n--;
    }
    m->off[m->n++] = off;
}

void pm_cut(PromptMarks *m, long cut)
{
    int i, k = 0;
    if (cut <= 0)
        return;
    for (i = 0; i < m->n; i++)
        if (m->off[i] >= cut)
            m->off[k++] = m->off[i] - cut;
    m->n = k;
}

long pm_prev(const PromptMarks *m, long pos)
{
    int i;
    for (i = m->n - 1; i >= 0; i--)
        if (m->off[i] < pos)
            return m->off[i];
    return -1;
}

long pm_next(const PromptMarks *m, long pos)
{
    int i;
    for (i = 0; i < m->n; i++)
        if (m->off[i] > pos)
            return m->off[i];
    return -1;
}
