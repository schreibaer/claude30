#include "rules.h"
#include <string.h>

static const char *const kShells[] = {
    "sh", "bash", "zsh", "fish", "env", "sudo", "su", "xargs", "eval", "exec",
    "command", "nohup", "time", "python", "python3", "node", "ruby", "perl",
    "php", "osascript", "awk", "find", "curl", "wget", "npx", "docker", NULL
};
static const char *const kBareRefused[] = {
    "Bash", "Edit", "Write", "MultiEdit", "NotebookEdit", "WebFetch", "Agent", "Read", NULL
};
static const char *const kFileTools[] = {
    "Edit", "Write", "MultiEdit", "NotebookEdit", "Read", NULL
};
static const char *const kBadPaths[] = {
    ".claude/", ".mcp.json", ".github/", ".git", ".ssh/", "package.json", "makefile",
    ".gitlab-ci", ".circleci/", "jenkinsfile", ".env", "id_rsa", "id_ed25519", NULL
};

static int in_list(const char *const *list, const char *s, long n)
{
    int i;
    for (i = 0; list[i]; i++)
        if ((long)strlen(list[i]) == n && memcmp(list[i], s, n) == 0)
            return 1;
    return 0;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int starts_ci_len(const char *s, long n, const char *pre, long k)
{
    long i;
    if (n < k)
        return 0;
    for (i = 0; i < k; i++)
        if (lower(s[i]) != pre[i])
            return 0;
    return 1;
}

static int starts_ci(const char *s, long n, const char *pre)
{
    return starts_ci_len(s, n, pre, (long)strlen(pre));
}

static int ends_ci(const char *s, long n, const char *suf)
{
    long k = (long)strlen(suf), i;
    if (n < k)
        return 0;
    for (i = 0; i < k; i++)
        if (lower(s[n - k + i]) != suf[i])
            return 0;
    return 1;
}

/* A path whose name could hold secrets or change how the project builds. */
static int hidden_path(const char *s, long n)
{
    long start = 0, i, j;
    int b;
    for (i = 0; i <= n; i++) {
        if (i == n || s[i] == '/') {
            long len = i - start;
            const char *seg = s + start;
            if (starts_ci(seg, len, ".env") || ends_ci(seg, len, ".pem")
                    || ends_ci(seg, len, ".key") || starts_ci(seg, len, "id_rsa")
                    || starts_ci(seg, len, "id_ed25519"))
                return 1;
            start = i + 1;
        }
    }
    for (b = 0; kBadPaths[b]; b++) {
        long k = (long)strlen(kBadPaths[b]);
        long k0 = k - (kBadPaths[b][k - 1] == '/' ? 1 : 0);
        if (n == k0 && starts_ci_len(s, n, kBadPaths[b], k0))
            return 1;
        for (j = 0; j + k <= n; j++)
            if ((j == 0 || s[j - 1] == '/') && starts_ci(s + j, n - j, kBadPaths[b]))
                return 1;
    }
    return 0;
}

static int has_dotdot(const char *s, long n)
{
    long i;
    for (i = 0; i + 1 < n; i++)
        if (s[i] == '.' && s[i + 1] == '.')
            return 1;
    return 0;
}

static int word_chars(const char *s, long n)
{
    long i;
    int alnum = 0;
    if (n == 0)
        return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            alnum = 1;
        else if (c != '_')
            return 0;
    }
    return alnum;
}

const char *rule_check(const char *rule, long n)
{
    long i, toolLen, clen, plen;
    const char *paren, *content, *prefix;
    int hasContent;

    if (n > RULE_MAX)
        return "The rule is longer than 200 characters.";
    for (i = 0; i < n; i++)
        if ((unsigned char)rule[i] < 32 || (unsigned char)rule[i] > 126)
            return "The rule may only hold printable ASCII.";
    paren = NULL;
    for (i = 0; i < n; i++)
        if (rule[i] == '(') {
            paren = rule + i;
            break;
        }
    if (!paren) {
        toolLen = n;
        hasContent = 0;
        content = rule;
        clen = 0;
        if (!word_chars(rule, n))
            return "The rule must look like Tool or Tool(what it covers).";
    } else {
        toolLen = paren - rule;
        if (rule[n - 1] != ')' || n - toolLen < 3 || !word_chars(rule, toolLen))
            return "The rule must look like Tool or Tool(what it covers).";
        hasContent = 1;
        content = paren + 1;
        clen = n - toolLen - 2;
    }
    if (toolLen == 12 && memcmp(rule, "ExitPlanMode", 12) == 0)
        return "A rule cannot skip plan review.";
    if (toolLen >= 5 && memcmp(rule, "mcp__", 5) == 0) {
        long us = 0;
        for (i = 5; i + 1 < toolLen; i++)
            if (rule[i] == '_' && rule[i + 1] == '_')
                us++;
        if (!hasContent && us < 1)
            return "A rule for a whole MCP server is too broad.";
        return NULL;
    }
    if (!hasContent) {
        if (in_list(kBareRefused, rule, toolLen))
            return "That tool needs a narrower rule than all of it.";
        return NULL;
    }
    if ((clen == 1 && content[0] == '*') || (clen == 2 && memcmp(content, ":*", 2) == 0)
            || (clen == 2 && memcmp(content, "**", 2) == 0)
            || (clen == 3 && memcmp(content, "/**", 3) == 0) || content[0] == '*')
        return "The rule covers everything.";
    if (toolLen == 4 && memcmp(rule, "Bash", 4) == 0) {
        long fw = 0;
        plen = clen;
        /* A prefix wildcard: the CLI writes "npm test *", older ones "npm test:*". */
        if (plen >= 2 && (content[plen - 2] == ':' || content[plen - 2] == ' ')
                && content[plen - 1] == '*')
            plen -= 2;
        prefix = content;
        while (fw < plen && prefix[fw] == ' ')
            fw++;
        if (fw == plen)
            return "The rule covers everything.";
        for (i = 0; i < plen; i++) {
            char c = prefix[i];
            if (c == ';' || c == '&' || c == '|' || c == '>' || c == '<' || c == '`'
                    || c == '\n' || (c == '$' && i + 1 < plen && prefix[i + 1] == '('))
                return "A command prefix cannot hold shell operators.";
        }
        {
            long s0 = fw, e0 = fw;
            while (e0 < plen && prefix[e0] != ' ')
                e0++;
            if (memchr(prefix + s0, '/', e0 - s0))
                return "The command must start with a plain program name.";
            if (memchr(prefix + s0, '=', e0 - s0))
                return "The command cannot start with a variable setting.";
            {
                char word[32];
                long w;
                if (e0 - s0 < (long)sizeof(word)) {
                    for (w = 0; w < e0 - s0; w++)
                        word[w] = lower(prefix[s0 + w]);
                    if (in_list(kShells, word, e0 - s0))
                        return "That program can run anything.";
                }
            }
            if (memchr(prefix, '*', plen) || memchr(prefix, '?', plen))
                return "A command prefix cannot hold wildcards.";
        }
        return NULL;
    }
    if (in_list(kFileTools, rule, toolLen)) {
        if (content[0] == '/' || content[0] == '~' || has_dotdot(content, clen))
            return "Paths must stay inside the project.";
        {
            long body = clen, lastSlash = -1;
            if (clen >= 3 && memcmp(content + clen - 3, "/**", 3) == 0)
                body = clen - 3;
            for (i = 0; i < clen; i++)
                if (content[i] == '?' || content[i] == '[' || content[i] == ']'
                        || content[i] == '{' || content[i] == '}')
                    return "Only * may be used in a path.";
            for (i = 0; i < body; i++)
                if (content[i] == '/')
                    lastSlash = i;
            for (i = 0; i < lastSlash; i++)
                if (content[i] == '*')
                    return "A * may only be in the last part of the path.";
        }
        if (hidden_path(content, clen))
            return "That path holds secrets or can change how the project runs.";
    }
    return NULL;
}
