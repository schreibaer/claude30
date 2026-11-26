#include "tabs.h"
#include <string.h>
#include "window.h"

static Tab gTabs[MAX_TABS];      /* by session id - 1 */
static short gOrder[MAX_TABS];   /* strip order: indexes into gTabs */
static short gCount;
static Tab *gFront;

Tab *tabs_add(const char *project, short sid)
{
    Tab *t;
    short at, i;
    if (sid < 1 || sid > MAX_TABS || gTabs[sid - 1].used || gCount >= MAX_TABS)
        return NULL;
    t = &gTabs[sid - 1];
    memset(t, 0, sizeof(*t));
    t->used = true;
    t->sid = sid;
    strncpy(t->project, project, TAB_NAME_MAX);
    batch_init(&t->batch);
    batch_set_rule(&t->batch, window_rule_len());
    al_init(&t->agents);
    fold_init(&t->folds);
    t->agent_pending = -1;
    at = gFront ? tabs_index(gFront) + 1 : gCount;
    for (i = gCount; i > at; i--)
        gOrder[i] = gOrder[i - 1];
    gOrder[at] = (short)(sid - 1);
    gCount++;
    return t;
}

void tabs_remove(Tab *t)
{
    short at = tabs_index(t), i;
    if (at < 0)
        return;
    for (i = at; i < gCount - 1; i++)
        gOrder[i] = gOrder[i + 1];
    gCount--;
    batch_free(&t->batch);
    fold_free(&t->folds);
    log_close(&t->log);
    permdlg_free(&t->perm);
    t->used = false;
    if (gFront == t)
        gFront = gCount == 0 ? NULL : tabs_at(at > 0 ? at - 1 : 0);
}

short tabs_count(void)
{
    return gCount;
}

Tab *tabs_at(short index)
{
    if (index < 0 || index >= gCount)
        return NULL;
    return &gTabs[gOrder[index]];
}

short tabs_index(const Tab *t)
{
    short i;
    for (i = 0; i < gCount; i++)
        if (&gTabs[gOrder[i]] == t)
            return i;
    return -1;
}

Tab *tabs_by_sid(short sid)
{
    if (sid < 1 || sid > MAX_TABS || !gTabs[sid - 1].used)
        return NULL;
    return &gTabs[sid - 1];
}

Tab *tabs_by_project(const char *project)
{
    short i;
    for (i = 0; i < gCount; i++)
        if (!gTabs[gOrder[i]].chat && strcmp(gTabs[gOrder[i]].project, project) == 0)
            return &gTabs[gOrder[i]];
    return NULL;
}

Tab *tabs_front(void)
{
    return gFront;
}

void tabs_set_front(Tab *t)
{
    gFront = t;
}
