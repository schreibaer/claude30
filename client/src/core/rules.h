/* Which allow rules may be saved from the permission window. The same rules
   as validate_rule in bridge.py (PROTOCOL.md, "Rules (rules)"); the bridge's
   check is the authority, this one lets the person see the reason before
   anything is sent. Portable C, no Toolbox. */
#ifndef RULES_H
#define RULES_H

#define RULE_MAX 200
#define RULE_LEN (RULE_MAX + 8)   /* a rule with its NUL, with room to spare */

/* NULL when the rule may be saved, else a short reason (static text). */
const char *rule_check(const char *rule, long n);

#endif
