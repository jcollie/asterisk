#ifndef _ASTERISK_CHANVARS_INCLUDE
#define _ASTERISK_CHANVARS_INCLUDE

#include <asterisk/linkedlists.h>

struct ast_var_t {
	char *name;
	char *value;
	AST_LIST_ENTRY(ast_var_t) entries;
};

struct ast_var_t *ast_var_assign(char *name,char *value);
void ast_var_delete(struct ast_var_t *var);
char *ast_var_name(struct ast_var_t *var);
char *ast_var_value(struct ast_var_t *var);

#endif
