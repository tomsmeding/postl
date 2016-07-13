#pragma once

typedef enum postl_valtype_t{
	POSTL_NUM,
	POSTL_STR,
	POSTL_BLOCK,
	//POSTL_ARR,
	//POSTL_SENTINEL,
} postl_valtype_t;

struct code_t;
typedef struct code_t code_t;

typedef struct postl_stackval_t{
	postl_valtype_t type;
	double numv;
	char *strv; //owner is this stackval
	code_t *blockv;
} postl_stackval_t;

struct postl_program_t;
typedef struct postl_program_t postl_program_t;


postl_program_t* postl_makeprogram(void);
void postl_register(postl_program_t *prog,const char *name,void (*func)(postl_program_t*));
const char* postl_addcode(postl_program_t *prog,const char *source); //maybe returns error string
const char* postl_runglobalcode(postl_program_t *prog); //maybe returns error string

postl_stackval_t postl_stackval_makenum(int num);
postl_stackval_t postl_stackval_makestr(const char *str);

int postl_stack_size(postl_program_t *prog);
void postl_stack_push(postl_program_t *prog,postl_stackval_t val);
void postl_stack_pushes(postl_program_t *prog,int nvals,const postl_stackval_t *vals);
postl_stackval_t postl_stack_pop(postl_program_t *prog); //returned stackval must be released!

void postl_stackval_release(postl_program_t *prog,postl_stackval_t val);

const char* postl_callfunction(postl_program_t *prog,const char *name); //maybe returns error string (at least valid till next call to this function)
void postl_destroy(postl_program_t *prog);
