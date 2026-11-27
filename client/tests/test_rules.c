/* The rule check: the same accept and refuse tables as the bridge's tests
   (fixtures/rules_allowed.txt and rules_refused.txt), so both sides agree. */
#include "check.h"
#include "rules.h"
#include <stdio.h>

static int each_line(const char *name, int expect_ok)
{
    char path[512], line[512];
    FILE *f;
    int n = 0;
    snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
    f = fopen(path, "r");
    CHECK(f != NULL);
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        size_t k = strlen(line);
        const char *why;
        while (k > 0 && (line[k - 1] == '\n' || line[k - 1] == '\r'))
            line[--k] = 0;
        if (!k)
            continue;
        why = rule_check(line, (long)k);
        if (expect_ok != (why == NULL))
            fprintf(stderr, "%s rule: %s -> %s\n", expect_ok ? "allowed" : "refused", line,
                    why ? why : "accepted");
        CHECK((why == NULL) == expect_ok);
        if (!expect_ok && why)
            CHECK(strlen(why) > 8);
        n++;
    }
    fclose(f);
    return n;
}

static void test_tables(void)
{
    char big[320];
    CHECK(each_line("rules_allowed.txt", 1) >= 8);
    CHECK(each_line("rules_refused.txt", 0) >= 25);
    memset(big, 'a', sizeof(big));
    memcpy(big, "Bash(", 5);
    big[300] = ')';
    CHECK(rule_check(big, 301) != NULL);
}

static void test_reasons(void)
{
    CHECK(strstr(rule_check("Bash(sh:*)", 10), "anything") != NULL);
    CHECK(strstr(rule_check("ExitPlanMode", 12), "plan review") != NULL);
    CHECK(strstr(rule_check("Edit(../x)", 10), "inside the project") != NULL);
    CHECK(rule_check("Bash(echo (x):*)", 16) == NULL);
}

int main(void)
{
    test_tables();
    test_reasons();
    CHECK_DONE();
}
