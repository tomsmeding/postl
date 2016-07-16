#define _GNU_SOURCE  // asprintf
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <assert.h>

#include "postl.h"

#define malloc(n,t) (t*)malloc((n)*sizeof(t))

#if 0
#define DBG(...) __VA_ARGS__
#else
#define DBG(...)
#endif
#define DBGF(...) DBG(do {fprintf(stderr,__VA_ARGS__); fputc('\n',stderr);} while(0))

#define HASHMAP_SIZE (127)

#define GLOBALCODE_FUNCNAME "__global"


__attribute__((noreturn)) static void outofmem(void){
	fprintf(stderr,"postl: OUT OF MEMORY!\n");
	exit(1);
}

// Murmur3 performed only slightly better (3 collisions instead of 5 for the
// builtins) than this simple sum-of-characters.
// Keeping this until I find something significantly better.
static int namehash(const char *name){
	int h=0,len=strlen(name);
	for(int i=0;i<len;i++)h+=name[i];
	return h%HASHMAP_SIZE;
}

static const char* valtype_string(postl_valtype_t type){
	switch(type){
		case POSTL_NUM: return "POSTL_NUM";
		case POSTL_STR: return "POSTL_STR";
		case POSTL_BLOCK: return "POSTL_BLOCK";
		default: return "POSTL_???";
	}
}

static double realmodulo(double a,double b){
	if(b==0)return nan("");
	int sa=a<0?-1:1;
	a=fabs(a); b=fabs(b);
	return sa*(a-b*floor(a/b));
}

static void pprintstr(const char *str){
	putchar('"');
	for(const char *p=str;*p;p++){
		if(*p=='\n')printf("\\n");
		else if(*p=='\r')printf("\\r");
		else if(*p=='\t')printf("\\t");
		else if(*p=='\\')printf("\\\\");
		else if(*p=='"')printf("\\\"");
		else if(*p<32||*p>126)printf("\\x%02X",*p);
		else putchar(*p);
	}
	putchar('"');
}


typedef enum tokentype_t{
	TT_NUM,
	TT_STR,
	TT_WORD,
	TT_PPC, // preprocessor command
	TT_SYMBOL
} tokentype_t;

typedef struct token_t{
	tokentype_t type;
	char *str;
} token_t;

typedef struct code_t{
	int sz,len;
	token_t *tokens; // NULL iff sz==0
} code_t;


typedef struct stackitem_t{
	postl_stackval_t val;
	struct stackitem_t *next;
} stackitem_t;


typedef struct funcmap_item_t{
	char *name;
	void (*cfunc)(postl_program_t*); // NULL if not applicable
	code_t code; // sz==0 if not applicable
} funcmap_item_t;

typedef struct funcmap_llitem_t{
	funcmap_item_t item;
	struct funcmap_llitem_t *next;
} funcmap_llitem_t;


/*typedef struct var_item_t{
	char *name;
	postl_stackval_t val;
} var_item_t;

typedef struct var_llitem_t{
	var_item_t item;
	struct var_llitem_t *next;
} var_llitem_t;*/


typedef struct name_llitem_t{
	char *name;
	struct name_llitem_t *next;
} name_llitem_t;

typedef struct scope_frame_t{
	name_llitem_t *vars[HASHMAP_SIZE];
	struct scope_frame_t *next;
} scope_frame_t;


struct postl_program_t{
	stackitem_t *stack;
	int stacksz;
	funcmap_llitem_t *fmap[HASHMAP_SIZE]; // the same function might appear multiple times after another,
	                                      // meaning it appears in multiple stacked scopes (the first
	                                      // appearance is always active)
	//var_llitem_t *vmap[HASHMAP_SIZE];
	code_t *buildblock; // !NULL iff collecting tokens in a { block }
	int blockdepth;
	scope_frame_t *scopestack;
};


static bool istruthy(postl_stackval_t val){
	switch(val.type){
		case POSTL_NUM: return val.numv!=0; break;
		case POSTL_STR: return val.strv[0]!='\0'; break;
		case POSTL_BLOCK: return true; break;
		default: assert(false);
	}
}


static void printstackval(postl_stackval_t val,bool pretty){
	switch(val.type){
		case POSTL_NUM:
			printf("%g",val.numv);
			break;
		case POSTL_STR:
			if(pretty)pprintstr(val.strv);
			else printf("%s",val.strv);
			break;
		case POSTL_BLOCK:{
			token_t *tokens=val.blockv->tokens;
			printf("{ ");
			for(int i=0;i<val.blockv->len;i++){
				if(tokens[i].type==TT_STR)pprintstr(tokens[i].str);
				else printf("%s",tokens[i].str);
				putchar(' ');
			}
			putchar('}');
			break;
		}
	}
	fflush(stdout);
}


static void funcmap_item_release(funcmap_item_t item){
	free(item.name);
	if(item.code.sz!=0){
		token_t *tokens=item.code.tokens;
		for(int i=0;i<item.code.len;i++){
			free(tokens[i].str);
		}
		free(tokens);
	}
}


//maybe returns error string
static const char* tokenise(token_t **tokensp,const char *source,int *ntokens){
	static char errbuf[256];
	*tokensp=NULL; // precaution
	int sourcelen=strlen(source);
	int sz=128,len=0;
	token_t *tokens=malloc(sz,token_t);
	if(!tokens)outofmem();

	int blockdepth=0;

#define DESTROY_TOKENS_RETF(...) \
		do { \
			for(int i=0;i<len;i++)free(tokens[i].str); \
			free(tokens); \
			snprintf(errbuf,256,__VA_ARGS__); \
			return errbuf; \
		} while(0)

	for(int i=0;i<sourcelen;i++){
		if(strchr(" \t\n\r",source[i])!=NULL){
			//whitespace; pass
		} else if(source[i]=='#'){ // comment
			i++;
			while(i<sourcelen&&source[i]!='\n')i++;
		} else if(isdigit(source[i])||(i<sourcelen-1&&source[i]=='-'&&isdigit(source[i+1]))){
			char *endp;
			double nval=strtod(source+i,&endp);
			int numlen=endp-(source+i);
			if(isnan(nval)||isinf(nval)||numlen<=0)
				DESTROY_TOKENS_RETF("postl: Invalid number in source");

			if(len==sz&&(sz*=2,tokens=realloc(tokens,sz*sizeof(token_t)))==NULL)outofmem();
			tokens[len].type=TT_NUM;
			tokens[len].str=malloc(numlen+1,char);
			if(!tokens[len].str)outofmem();
			memcpy(tokens[len].str,source+i,numlen);
			tokens[len].str[numlen]='\0';
			len++;
			i+=numlen-1;
		} else if(source[i]=='"'){
			i++;
			int j,slen=0;
			for(j=i;j<sourcelen;j++){
				if(source[j]=='"')break;
				if(source[j]=='\\')j++;
				slen++;
			}
			if(j==sourcelen)
				DESTROY_TOKENS_RETF("postl: Unclosed string (from char %d) in source file",i-1);

			if(len==sz&&(sz*=2,tokens=realloc(tokens,sz*sizeof(token_t)))==NULL)outofmem();
			tokens[len].type=TT_STR;
			tokens[len].str=malloc(slen+1,char);
			if(!tokens[len].str)outofmem();
			int k=0;
			for(j=i;;j++){
				if(source[j]=='"')break;
				if(source[j]=='\\'){
					j++;
					char c;
					switch(source[j]){
						case 'a': c='\x07'; break;
						case 'b': c='\x08'; break;
						case 'n': c='\n'; break;
						case 'r': c='\r'; break;
						case 't': c='\t'; break;
						default: c=source[j]; break;
						// TODO: \unnnn and \Unnnnnnnn
						// purposefully skipped: \f and \v
					}
					tokens[len].str[k++]=c;
				} else tokens[len].str[k++]=source[j];
			}
			tokens[len].str[k]='\0';
			len++;
			i=j;
		} else if(isalpha(source[i])||source[i]=='_'||source[i]=='@'){
			bool isppc=source[i]=='@';
			if(isppc){
				i++;
				if(i==sourcelen||(!isalpha(source[i])&&source[i]!='_'&&!isdigit(source[i])))
					DESTROY_TOKENS_RETF("postl: '@' not followed by PPC token");
			}
			int j;
			for(j=i+1;j<sourcelen;j++){
				if(!isalpha(source[j])&&source[j]!='_'&&!isdigit(source[j]))break;
			}
			int wordlen=j-i;

			if(len==sz&&(sz*=2,tokens=realloc(tokens,sz*sizeof(token_t)))==NULL)outofmem();
			tokens[len].type=isppc?TT_PPC:TT_WORD;
			tokens[len].str=malloc(wordlen+1,char);
			if(!tokens[len].str)outofmem();
			memcpy(tokens[len].str,source+i,wordlen);
			tokens[len].str[wordlen]='\0';
			len++;
			i=j-1;
		} else /*if(strchr("+*-/%~&|><={}",source[i])!=NULL)*/{
			if(len==sz&&(sz*=2,tokens=realloc(tokens,sz*sizeof(token_t)))==NULL)outofmem();
			tokens[len].type=TT_SYMBOL;
			tokens[len].str=malloc(2,char);
			if(!tokens[len].str)outofmem(); //rlly
			tokens[len].str[0]=source[i];
			tokens[len].str[1]='\0';
			len++;
			if(source[i]=='{')blockdepth++;
			else if(source[i]=='}')blockdepth--;
			if(blockdepth<0)
				DESTROY_TOKENS_RETF("postl: Extra '}' in source");
		} //else DESTROY_TOKENS_RET_MIN1;
		DBGF("i=%d, len=%d; tokens[%d].type=%d",i,len,len==0?-123:len-1,tokens[len==0?-123:len-1].type);
	}

	if(blockdepth<0)
		DESTROY_TOKENS_RETF("postl: Extra '}' in source");
	if(blockdepth>0)
		DESTROY_TOKENS_RETF("postl: Missing %d '{'%s in source",blockdepth,blockdepth==1?"":"s");

#undef DESTROY_TOKENS_RETF

	*tokensp=tokens;
	*ntokens=len;
	return NULL;
}

static const char* execute_token(postl_program_t *prog,token_t token){
	if(prog->buildblock){
		if(strcmp(token.str,"}")==0&&--prog->blockdepth==0){
			code_t *bb=prog->buildblock;
			if(bb->len==bb->sz){
				bb->sz++;
				bb->tokens=realloc(bb->tokens,bb->sz*sizeof(token_t));
			}
			bb->tokens[bb->len].type=TT_WORD;
			asprintf(&bb->tokens[bb->len].str,"scopeleave");
			if(!bb->tokens[bb->len].str)outofmem();
			bb->len++;

			stackitem_t *si=malloc(1,stackitem_t);
			if(!si)outofmem();
			si->val.type=POSTL_BLOCK;
			si->val.strv=NULL;
			si->val.blockv=bb;
			prog->buildblock=NULL;
			si->next=prog->stack;
			prog->stack=si;
			prog->stacksz++;
		} else {
			code_t *bb=prog->buildblock;
			if(bb->len==bb->sz){
				bb->sz*=2;
				bb->tokens=realloc(bb->tokens,bb->sz);
				if(!bb->tokens)outofmem();
			}
			bb->tokens[bb->len].type=token.type;
			asprintf(&bb->tokens[bb->len].str,"%s",token.str);
			if(!bb->tokens[bb->len].str)outofmem();
			bb->len++;
			if(strcmp(token.str,"{")==0)prog->blockdepth++;
		}
		return NULL;
	}
	switch(token.type){
		case TT_NUM:{
			double d=strtod(token.str,NULL);
			stackitem_t *si=malloc(1,stackitem_t);
			si->val.type=POSTL_NUM;
			si->val.numv=d;
			si->val.strv=NULL;
			si->val.blockv=NULL;
			si->next=prog->stack;
			prog->stack=si;
			prog->stacksz++;
			break;
		}
		case TT_STR:{
			stackitem_t *si=malloc(1,stackitem_t);
			si->val.type=POSTL_STR;
			asprintf(&si->val.strv,"%s",token.str);
			if(!si->val.strv)outofmem();
			si->val.blockv=NULL;
			si->next=prog->stack;
			prog->stack=si;
			prog->stacksz++;
			break;
		}
		case TT_PPC:
			return "No preprocessor commands known";
		case TT_WORD:
		case TT_SYMBOL:
			return postl_callfunction(prog,token.str);
	}
	return NULL;
}

// maybe returns error string
static const char* execute_block(postl_program_t *prog,code_t block){
	for(int i=0;i<block.len;i++){
		const char *errstr=execute_token(prog,block.tokens[i]);
		if(errstr)return errstr;
	}
	return NULL;
}

// returns whether the function existed
static bool deletefunction(postl_program_t *prog,const char *name){
	int h=namehash(name);
	funcmap_llitem_t *lli=prog->fmap[h],*parent=NULL;
	while(lli){
		if(strcmp(lli->item.name,name)==0)break;
		parent=lli;
		lli=lli->next;
	}
	if(!lli)return false;
	if(parent==NULL)prog->fmap[h]=lli->next;
	else parent->next=lli->next;
	free(lli->item.name);
	if(lli->item.code.sz){
		for(int i=0;i<lli->item.code.len;i++){
			free(lli->item.code.tokens[i].str);
		}
		free(lli->item.code.tokens);
	}
	free(lli);
	return true;
}

typedef enum builtin_enum_t{
	BI_PLUS, BI_MINUS, BI_TIMES, BI_DIVIDE, BI_MODULO,
	BI_EQ, BI_GT, BI_LT,
	BI_NOT,
	BI_PRINT, BI_LF,
	BI_BLOCKOPEN, /*BI_BLOCKCLOSE,*/ //blockclose is directly handled by execute_token
	BI_DEF, BI_GDEF,
	BI_EVAL,
	BI_BUILTIN,
	BI_SWAP, BI_DUP, BI_POP, BI_ROLL, BI_ROTATE,
	BI_IF, BI_WHILE, BI_IFELSE,
	BI_STACKSIZE,
	BI_STACKDUMP,
	BI_CEIL, BI_FLOOR, BI_ROUND, BI_MIN, BI_MAX, BI_ABS, BI_SQRT, BI_EXP, BI_LOG, BI_POW,
	BI_SIN, BI_COS, BI_TAN, BI_ASIN, BI_ACOS, BI_ATAN, BI_ATAN2, BI_E, BI_PI,
	BI_SCOPEENTER, BI_SCOPELEAVE
} builtin_enum_t;

typedef struct builtin_llitem_t {
	builtin_enum_t id;
	const char *name;
	struct builtin_llitem_t *next;
} builtin_llitem_t;

static builtin_llitem_t *builtins_hmap[HASHMAP_SIZE]={NULL};
static bool builtins_hmap_initialised=false;

static void builtin_add(const char *name,builtin_enum_t id){
	int h=namehash(name);
	builtin_llitem_t *lli=malloc(1,builtin_llitem_t);
	if(!lli)outofmem();
	lli->id=id;
	lli->name=name;
	lli->next=builtins_hmap[h];
	builtins_hmap[h]=lli;
}

static void initialise_builtins_hmap(void){
	builtin_add("+",         BI_PLUS);
	builtin_add("-",         BI_MINUS);
	builtin_add("*",         BI_TIMES);
	builtin_add("/",         BI_DIVIDE);
	builtin_add("%",         BI_MODULO);
	builtin_add("=",         BI_EQ);
	builtin_add(">",         BI_GT);
	builtin_add("<",         BI_LT);
	builtin_add("!",         BI_NOT);
	builtin_add("print",     BI_PRINT);
	builtin_add("lf",        BI_LF);
	builtin_add("{",         BI_BLOCKOPEN);
	builtin_add("def",       BI_DEF);
	builtin_add("gdef",      BI_GDEF);
	builtin_add("eval",      BI_EVAL);
	builtin_add("builtin",   BI_BUILTIN);
	builtin_add("swap",      BI_SWAP);
	builtin_add("dup",       BI_DUP);
	builtin_add("pop",       BI_POP);
	builtin_add("roll",      BI_ROLL);
	builtin_add("rotate",    BI_ROTATE);
	builtin_add("if",        BI_IF);
	builtin_add("while",     BI_WHILE);
	builtin_add("ifelse",    BI_IFELSE);
	builtin_add("stacksize", BI_STACKSIZE);
	builtin_add("stackdump", BI_STACKDUMP);
	builtin_add("ceil",      BI_CEIL);
	builtin_add("floor",     BI_FLOOR);
	builtin_add("round",     BI_ROUND);
	builtin_add("min",       BI_MIN);
	builtin_add("max",       BI_MAX);
	builtin_add("abs",       BI_ABS);
	builtin_add("sqrt",      BI_SQRT);
	builtin_add("exp",       BI_EXP);
	builtin_add("log",       BI_LOG);
	builtin_add("pow",       BI_POW);
	builtin_add("sin",       BI_SIN);
	builtin_add("cos",       BI_COS);
	builtin_add("tan",       BI_TAN);
	builtin_add("asin",      BI_ASIN);
	builtin_add("acos",      BI_ACOS);
	builtin_add("atan",      BI_ATAN);
	builtin_add("atan2",     BI_ATAN2);
	builtin_add("E",         BI_E);
	builtin_add("PI",        BI_PI);
	builtin_add("scopeenter",BI_SCOPEENTER);
	builtin_add("scopeleave",BI_SCOPELEAVE);

	builtins_hmap_initialised=true;
}

static const char* execute_builtin(postl_program_t *prog,const char *name,bool *found){
	static char errbuf[256];
	DBGF("execute_builtin(%p,%s,%p)",prog,name,found);
	builtin_llitem_t *lli=builtins_hmap[namehash(name)];
	while(lli){
		if(strcmp(lli->name,name)==0)break;
		lli=lli->next;
	}
	if(found)*found=(bool)lli;
	if(!lli)return NULL;

#define RETURN_WITH_ERROR(...) \
		do { \
			snprintf(errbuf,256,__VA_ARGS__); \
			return errbuf; \
		} while(0)

#define STACKSIZE_CHECK(n) \
		do if(prog->stacksz<n) \
			RETURN_WITH_ERROR("postl: builtin '%s' needs %d argument%s, but got %d", \
				name,n,n==1?"":"s",prog->stacksz); \
		while(0)

#define CANNOT_USE(type) RETURN_WITH_ERROR("postl: Cannot use %s in '%s'",valtype_string((type)),(name))

	postl_stackval_t a,b;
	postl_stackval_t res;

	switch(lli->id){

#define BINARY_ARITH_OP(id,expr) \
		case (id): STACKSIZE_CHECK(2); \
			b=postl_stack_pop(prog); \
			a=postl_stack_pop(prog); \
			if(a.type!=POSTL_NUM||b.type!=POSTL_NUM){ \
				postl_stackval_release(a); \
				postl_stackval_release(b); \
				if(a.type!=POSTL_NUM)CANNOT_USE(a.type); \
				else CANNOT_USE(b.type); \
			} \
			res.type=POSTL_NUM; \
			res.numv=(expr); \
			postl_stack_push(prog,res); \
			postl_stackval_release(a); \
			postl_stackval_release(b); \
			break;

#define UNARY_ARITH_OP(id,expr) \
		case (id): STACKSIZE_CHECK(1); \
			a=postl_stack_pop(prog); \
			if(a.type!=POSTL_NUM){ \
				postl_stackval_release(a); \
				CANNOT_USE(a.type); \
			} \
			res.type=POSTL_NUM; \
			res.numv=(expr); \
			postl_stack_push(prog,res); \
			postl_stackval_release(a); \
			break;


		case BI_PLUS: STACKSIZE_CHECK(2);
			b=postl_stack_pop(prog);
			a=postl_stack_pop(prog);
			if(a.type!=b.type){
				postl_stackval_release(a);
				postl_stackval_release(b);
				RETURN_WITH_ERROR("postl: Builtin '+' needs arguments of similar types (%s != %s)",
					valtype_string(a.type),valtype_string(b.type));
			}
			if(a.type==POSTL_BLOCK){
				postl_stackval_release(a);
				postl_stackval_release(b);
				CANNOT_USE(POSTL_BLOCK);
			} else if(a.type==POSTL_STR){
				res.type=POSTL_STR;
				asprintf(&res.strv,"%s%s",a.strv,b.strv);
				if(!res.strv)outofmem();
			} else {
				assert(a.type==POSTL_NUM);
				res.type=POSTL_NUM;
				res.numv=a.numv+b.numv;
			}
			postl_stack_push(prog,res);
			postl_stackval_release(a);
			postl_stackval_release(b);
			break;

		BINARY_ARITH_OP(BI_MINUS,a.numv-b.numv)
		BINARY_ARITH_OP(BI_TIMES,a.numv*b.numv)
		BINARY_ARITH_OP(BI_DIVIDE,b.numv==0?nan(""):a.numv/b.numv)
		BINARY_ARITH_OP(BI_MODULO,realmodulo(a.numv,b.numv))
		BINARY_ARITH_OP(BI_EQ,a.numv==b.numv)
		BINARY_ARITH_OP(BI_GT,a.numv>b.numv)
		BINARY_ARITH_OP(BI_LT,a.numv<b.numv)

		case BI_NOT: STACKSIZE_CHECK(1);
			a=postl_stack_pop(prog);
			b.type=POSTL_NUM;
			b.numv=!istruthy(a);
			postl_stackval_release(a);
			postl_stack_push(prog,b);
			break;

		case BI_PRINT: STACKSIZE_CHECK(1);
			a=postl_stack_pop(prog);
			printstackval(a,false);
			postl_stackval_release(a);
			break;

		case BI_LF:
			putchar('\n');
			break;

		case BI_BLOCKOPEN:
			assert(!prog->buildblock);
			code_t *bb=prog->buildblock=malloc(1,code_t);
			if(!bb)outofmem();
			bb->sz=128;
			bb->len=1;  // for the scopeenter
			bb->tokens=malloc(bb->sz,token_t);
			if(!bb->tokens)outofmem();
			bb->tokens[0].type=TT_WORD;
			asprintf(&bb->tokens[0].str,"scopeenter");
			if(!bb->tokens[0].str)outofmem();
			prog->blockdepth=1;
			break;

		case BI_DEF:
		case BI_GDEF:{
			STACKSIZE_CHECK(2);
			b=postl_stack_pop(prog);
			a=postl_stack_pop(prog);
			if(b.type!=POSTL_STR){
				postl_stackval_release(a);
				postl_stackval_release(b);
				RETURN_WITH_ERROR("postl: Second argument to '%s' should be string, is %s",
					name,valtype_string(b.type));
			}
			int h=namehash(b.strv);

			bool thisscope=true; // Whether this name is in the top scope; if so, we need to delete it
			                     // upon setting the new value
			if(prog->scopestack&&lli->id==BI_DEF){ // gdef does *not* check scoping
				name_llitem_t *nlli=prog->scopestack->vars[h];
				while(nlli){
					if(strcmp(nlli->name,b.strv)==0)break;
					nlli=nlli->next;
				}
				thisscope=(bool)nlli;
			}

			DBGF("[%s]: b.strv='%s'; thisscope=%d\n",name,b.strv,thisscope);
			if(thisscope){
				// Lookup the name in the function table, it might still need to be deleted
				funcmap_llitem_t *lli=prog->fmap[h],*parent=NULL;
				while(lli){
					if(strcmp(lli->item.name,b.strv)==0)break;
					parent=lli;
					lli=lli->next;
				}
				if(lli){
					if(parent)parent->next=lli->next;
					else prog->fmap[h]=lli->next;
					funcmap_item_release(lli->item);
					free(lli);
				}
			} else if(prog->scopestack){
				// The name was not declared in this scope, and we're not in global scope; add
				// the name to the scope's var list
				name_llitem_t *nlli=malloc(1,name_llitem_t);
				if(!nlli)outofmem();
				asprintf(&nlli->name,"%s",b.strv);
				if(!nlli->name)outofmem();
				nlli->next=prog->scopestack->vars[h];
				prog->scopestack->vars[h]=nlli;
			}

			if(a.type!=POSTL_BLOCK){
				if(a.type!=POSTL_NUM&&a.type!=POSTL_STR){
					postl_stackval_release(a);
					postl_stackval_release(b);
					RETURN_WITH_ERROR("postl: [DBG] Invalid a.type in BI_DEF: %d",a.type);
				}
				funcmap_llitem_t *lli=malloc(1,funcmap_llitem_t);
				if(!lli)outofmem();
				lli->item.name=b.strv;
				lli->item.cfunc=NULL;
				lli->item.code.sz=1;
				lli->item.code.len=1;
				lli->item.code.tokens=malloc(1,token_t);
				if(!lli->item.code.tokens)outofmem();
				token_t *token=lli->item.code.tokens;
				switch(a.type){
					case POSTL_NUM:
						token->type=TT_NUM;
						asprintf(&token->str,"%lf",a.numv);
						if(!token->str)outofmem();
						break;

					case POSTL_STR:
						token->type=TT_STR;
						asprintf(&token->str,"%s",a.strv);
						if(!token->str)outofmem();
						break;

					default:
						assert(false);
				}
				lli->next=prog->fmap[h];
				prog->fmap[h]=lli;
			} else {
				funcmap_llitem_t *lli=malloc(1,funcmap_llitem_t);
				if(!lli)outofmem();
				lli->item.name=b.strv;
				lli->item.cfunc=NULL;
				lli->item.code=*a.blockv;
				lli->next=prog->fmap[h];
				prog->fmap[h]=lli;
			}
			//postl_stackval_release(a); //values were copied to function definition
			//postl_stackval_release(b);
			break;
		}

		case BI_EVAL: STACKSIZE_CHECK(1);
			a=postl_stack_pop(prog);
			if(a.type!=POSTL_BLOCK){
				postl_stackval_release(a);
				CANNOT_USE(a.type);
			}
			const char *errstr=execute_block(prog,*a.blockv);
			postl_stackval_release(a);
			if(errstr)return errstr;
			break;

		case BI_BUILTIN:{ STACKSIZE_CHECK(1);
			a=postl_stack_pop(prog);
			if(a.type!=POSTL_STR){
				postl_stackval_release(a);
				CANNOT_USE(a.type);
			}
			if(strcmp(a.strv,"{")==0||strcmp(a.strv,"}")==0)
				return "Cannot call builtins '{' and '}' via builtin 'builtin'";
			bool found=false;
			const char *errstr=execute_builtin(prog,a.strv,&found);
			postl_stackval_release(a);
			if(errstr)return errstr;
			if(!found)
				RETURN_WITH_ERROR("postl: Builtin '%s' not found in builtin 'builtin'",
					a.strv);
			break;
		}

		case BI_SWAP:{ STACKSIZE_CHECK(2);
			stackitem_t *bsi=prog->stack,*asi=bsi->next;
			bsi->next=asi->next;
			asi->next=bsi;
			prog->stack=asi;
			break;
		}

		case BI_DUP: STACKSIZE_CHECK(1);
			postl_stack_push(prog,prog->stack->val);
			break;

		case BI_POP: STACKSIZE_CHECK(1);
			postl_stackval_release(postl_stack_pop(prog));
			break;

		// positive roll amount *extracts* elements from the stack bottom
		// negative roll amount *inserts* elements at the stack bottom
		// rotate: first cycle length, then rotation amount
		case BI_ROLL:
		case BI_ROTATE:{
			STACKSIZE_CHECK(1+(lli->id==BI_ROTATE));
			b=postl_stack_pop(prog);
			if(b.type!=POSTL_NUM){
				postl_stackval_release(b);
				CANNOT_USE(b.type);
			}
			if((double)(int)b.numv!=b.numv){
				postl_stackval_release(b);
				RETURN_WITH_ERROR("postl: Argument to '%s' not integral",name);
			}
			int bnum=b.numv;
			postl_stackval_release(b);
			int length;
			int stacksize;
			if(lli->id==BI_ROTATE){
				a=postl_stack_pop(prog);
				stacksize=prog->stacksz;
				if(a.type!=POSTL_NUM){
					postl_stackval_release(a);
					CANNOT_USE(a.type);
				}
				if((double)(int)a.numv!=a.numv){
					postl_stackval_release(a);
					RETURN_WITH_ERROR("postl: Argument to 'rotate' not integral");
				}
				length=a.numv;
				postl_stackval_release(a);
				if(length<0)
					RETURN_WITH_ERROR("postl: First argument to 'rotate' negative");
				if(length>stacksize)
					RETURN_WITH_ERROR("postl: First argument to 'rotate' larger than stacksize");
			} else {
				length=stacksize=prog->stacksz;
			}
			// DBGF("prelim: length=%d",length);
			if(stacksize<=1||length<=1)break;  // nothing to do
			int amount=(int)bnum%length;
			// DBGF("prelim: amount=%d",amount);
			if(amount<0)amount=length+amount;
			DBGF("ssize=%d length=%d amount=%d",stacksize,length,amount);

			stackitem_t *newbot=prog->stack;
			for(int i=0;i<length-amount-1;i++)newbot=newbot->next;
			stackitem_t *newtop=newbot->next;
			assert(newtop);  // because amount>0
			stackitem_t *bottom=newtop;
			for(int i=0;i<amount-1;i++)bottom=bottom->next;
			newbot->next=bottom->next;
			bottom->next=prog->stack;
			prog->stack=newtop;
			break;
		}

		case BI_IF:
		case BI_WHILE:{
			STACKSIZE_CHECK(2);
			postl_stackval_t body=postl_stack_pop(prog);
			if(body.type!=POSTL_BLOCK){
				postl_stackval_release(body);
				RETURN_WITH_ERROR("postl: Argument to '%s' should be block, is %s",
					name,valtype_string(body.type));
			}
			while(true){
				postl_stackval_t cond=postl_stack_pop(prog);
				bool stop=!istruthy(cond);
				postl_stackval_release(cond);
				if(stop)break;
				const char *errstr=execute_block(prog,*body.blockv);
				if(errstr){
					postl_stackval_release(body);
					return errstr;
				}
				if(lli->id==BI_IF)break;
			}
			postl_stackval_release(body);
			break;
		}

		case BI_IFELSE:{ STACKSIZE_CHECK(3);
			postl_stackval_t elsebl=postl_stack_pop(prog);
			if(elsebl.type!=POSTL_BLOCK){
				postl_stackval_release(elsebl);
				RETURN_WITH_ERROR("postl: Third argument to 'ifelse' should be block, is %s",
					valtype_string(elsebl.type));
			}
			postl_stackval_t thenbl=postl_stack_pop(prog);
			if(thenbl.type!=POSTL_BLOCK){
				postl_stackval_release(elsebl);
				postl_stackval_release(thenbl);
				RETURN_WITH_ERROR("postl: Second argument to 'ifelse' should be block, is %s",
					valtype_string(thenbl.type));
			}
			postl_stackval_t cond=postl_stack_pop(prog);
			bool condval=istruthy(cond);
			postl_stackval_release(cond);
			const char *errstr;
			if(condval){
				errstr=execute_block(prog,*thenbl.blockv);
			} else {
				errstr=execute_block(prog,*elsebl.blockv);
			}
			postl_stackval_release(thenbl);
			postl_stackval_release(elsebl);
			if(errstr)return errstr;
			break;
		}

		case BI_STACKSIZE:
			postl_stack_push(prog,postl_stackval_makenum(postl_stack_size(prog)));
			break;

		case BI_STACKDUMP:
			for(stackitem_t *si=prog->stack;si;si=si->next){
				printstackval(si->val,true);
				if(si->next)printf("  ");
			}
			putchar('\n');
			break;

		UNARY_ARITH_OP(BI_CEIL,ceil(a.numv))
		UNARY_ARITH_OP(BI_FLOOR,floor(a.numv))
		UNARY_ARITH_OP(BI_ROUND,round(a.numv))
		BINARY_ARITH_OP(BI_MIN,fmin(a.numv,b.numv))
		BINARY_ARITH_OP(BI_MAX,fmax(a.numv,b.numv))
		UNARY_ARITH_OP(BI_ABS,fabs(a.numv))
		UNARY_ARITH_OP(BI_SQRT,sqrt(a.numv))
		UNARY_ARITH_OP(BI_EXP,exp(a.numv))
		UNARY_ARITH_OP(BI_LOG,log(a.numv))
		BINARY_ARITH_OP(BI_POW,pow(a.numv,b.numv))
		UNARY_ARITH_OP(BI_SIN,sin(a.numv))
		UNARY_ARITH_OP(BI_COS,cos(a.numv))
		UNARY_ARITH_OP(BI_TAN,tan(a.numv))
		UNARY_ARITH_OP(BI_ASIN,asin(a.numv))
		UNARY_ARITH_OP(BI_ACOS,acos(a.numv))
		UNARY_ARITH_OP(BI_ATAN,atan(a.numv))
		BINARY_ARITH_OP(BI_ATAN2,atan2(a.numv,b.numv))

		case BI_E:
			postl_stack_push(prog,postl_stackval_makenum(M_E));
			break;

		case BI_PI:
			postl_stack_push(prog,postl_stackval_makenum(M_PI));
			break;

		case BI_SCOPEENTER:{
			scope_frame_t *frame=malloc(1,scope_frame_t);
			if(!frame)outofmem();
			for(int h=0;h<HASHMAP_SIZE;h++)frame->vars[h]=NULL;
			frame->next=prog->scopestack;
			prog->scopestack=frame;
			break;
		}

		case BI_SCOPELEAVE:{
			scope_frame_t *frame=prog->scopestack;
			if(!frame){
				snprintf(errbuf,256,"postl: scopeleave on empty scope stack");
				return errbuf;
			}
			prog->scopestack=frame->next;
			for(int h=0;h<HASHMAP_SIZE;h++){
				DBG(if(frame->vars[h])DBGF("h=%d:",h);)
				while(frame->vars[h]){
					DBGF("- '%s'",frame->vars[h]->name);
					deletefunction(prog,frame->vars[h]->name);
					free(frame->vars[h]->name);
					name_llitem_t *next=frame->vars[h]->next;
					free(frame->vars[h]);
					frame->vars[h]=next;
				}
			}
			break;
		}

		default:
			snprintf(errbuf,256,"postl: Sorry, not implemented: builtin '%s'",name);
			return errbuf;


#undef UNARY_ARITH_OP
#undef BINARY_ARITH_OP

	}

#undef CANNOT_USE
#undef STACKSIZE_CHECK
#undef RETURN_WITH_ERROR
	
	return NULL;
}


postl_program_t* postl_makeprogram(void){
	DBGF("postl_makeprogram()");
	postl_program_t *prog=malloc(1,postl_program_t);
	if(!prog)outofmem();
	prog->stack=NULL;
	prog->stacksz=0;
	for(int i=0;i<HASHMAP_SIZE;i++){
		prog->fmap[i]=NULL;
	}

	/*for(int i=0;i<HASHMAP_SIZE;i++){
		prog->vmap[i]=NULL;
	}*/

	prog->buildblock=NULL;
	prog->blockdepth=0; //not strictly necessary as buildblock==NULL conveys the message, but still

	prog->scopestack=NULL;

	initialise_builtins_hmap();

	return prog;
}

void postl_register(postl_program_t *prog,const char *name,void (*func)(postl_program_t*)){
	DBGF("postl_register(%p,%s,%p)",prog,name,func);
	int h=namehash(name);
	int len=strlen(name);
	funcmap_llitem_t *llitem=malloc(1,funcmap_llitem_t);
	if(!llitem)outofmem();
	llitem->item.name=malloc(len+1,char);
	if(!llitem->item.name)outofmem();
	memcpy(llitem->item.name,name,len+1);
	llitem->item.cfunc=func;
	llitem->item.code.sz=0;
	llitem->item.code.len=0;
	llitem->item.code.tokens=NULL;
	llitem->next=prog->fmap[h];
	prog->fmap[h]=llitem;
}

const char* postl_runcode(postl_program_t *prog,const char *source){
	DBGF("postl_runcode(%p,<<<\"%s\">>>)",prog,source);
	token_t *tokens=NULL;
	int len=-1;
	const char *errstr=tokenise(&tokens,source,&len);
	if(len<0){
		if(!errstr)return "postl: Tokenise error? (errstr=NULL)";
		return errstr;
	}
	if(errstr)return errstr;
	DBG(
		DBGF("%d tokens parsed",len);
		for(int i=0;i<len;i++){
			DBGF("token: type=%d '%s'",tokens[i].type,tokens[i].str);
		}
	)
	assert(tokens);

	errstr=NULL;
	for(int i=0;i<len;i++){
		errstr=execute_token(prog,tokens[i]);
		if(errstr)break;
	}
	for(int i=0;i<len;i++)free(tokens[i].str);
	free(tokens);
	assert(!prog->buildblock);
	return errstr;
}

postl_stackval_t postl_stackval_makenum(double num){
	DBGF("postl_stackval_makenum(%g)",num);
	postl_stackval_t st={.type=POSTL_NUM,.numv=num};
	return st;
}

postl_stackval_t postl_stackval_makestr(const char *str){
	DBGF("postl_stackval_makestr(%s)",str);
	int len=strlen(str);
	postl_stackval_t st={.type=POSTL_STR,.strv=malloc(len+1,char)};
	if(!st.strv)outofmem();
	memcpy(st.strv,str,len+1);
	return st;
}

int postl_stack_size(postl_program_t *prog){
	DBGF("postl_stack_size(%p)",prog);
	return prog->stacksz;
}

void postl_stack_push(postl_program_t *prog,postl_stackval_t val){
	DBGF("postl_stack_push(%p,{type=%d,...})",prog,val.type);
	stackitem_t *si=malloc(1,stackitem_t);
	if(!si)outofmem();
	si->val.type=val.type;
	si->val.numv=val.numv;
	if(val.type==POSTL_STR){
		if(val.strv==NULL){
			fprintf(stderr,"postl: NULL string in stack value to postl_stack_push\n");
			exit(1);
		}
		int len=strlen(val.strv);
		si->val.strv=malloc(len+1,char);
		if(!si->val.strv)outofmem();
		memcpy(si->val.strv,val.strv,len+1);
	} else si->val.strv=NULL;
	if(val.type==POSTL_BLOCK){
		if(val.blockv==NULL){
			fprintf(stderr,"postl: NULL block in stack value to postl_stack_push\n");
			exit(1);
		}
		code_t *code=malloc(1,code_t);
		if(!code)outofmem();
		code->sz=val.blockv->sz;
		code->len=val.blockv->len;
		code->tokens=malloc(code->sz,token_t);
		if(!code->tokens)outofmem();
		for(int i=0;i<code->len;i++){
			code->tokens[i].type=val.blockv->tokens[i].type;
			asprintf(&code->tokens[i].str,"%s",val.blockv->tokens[i].str);
			if(!code->tokens[i].str)outofmem();
		}
		si->val.blockv=code;
	} else si->val.blockv=NULL;
	si->next=prog->stack;
	prog->stack=si;
	prog->stacksz++;
}

void postl_stack_pushes(postl_program_t *prog,int nvals,const postl_stackval_t *vals){
	DBGF("postl_stack_pushes(%p,%d,%p)",prog,nvals,vals);
	for(int i=0;i<nvals;i++){
		postl_stack_push(prog,vals[i]);
	}
}

postl_stackval_t postl_stack_pop(postl_program_t *prog){
	DBGF("postl_stack_pop(%p)",prog);
	if(prog->stacksz==0){
		fprintf(stderr,"postl: Stack pop on empty stack!\n");
		exit(1);
	}
	postl_stackval_t val=prog->stack->val;
	stackitem_t *si=prog->stack;
	prog->stack=si->next;
	free(si);
	prog->stacksz--;
	return val;
}

void postl_stackval_release(postl_stackval_t val){
	DBGF("postl_stackval_release({type=%d,...})",val.type);
	if(val.type==POSTL_STR){
		if(!val.strv)return;
		free(val.strv);
	} else if(val.type==POSTL_BLOCK){
		if(!val.blockv)return;
		token_t *tokens=val.blockv->tokens;
		for(int i=0;i<val.blockv->len;i++){
			free(tokens[i].str);
		}
		free(tokens);
	}
}

const char* postl_callfunction(postl_program_t *prog,const char *name){
	static char errbuf[256]={'\0'};
	DBGF("postl_callfunction(%p,%s)",prog,name);

	int h=namehash(name);

	// Check for a user-defined function
	{
		funcmap_llitem_t *lli=prog->fmap[h];
		while(lli){
			if(strcmp(lli->item.name,name)==0)break;
			lli=lli->next;
		}
		if(lli!=NULL){
			DBGF("Calling '%s' -> user-defined function...",name);
			if(lli->item.cfunc){
				DBGF("'%s' is a C function",name);
				lli->item.cfunc(prog);
			} else {
				DBGF("'%s' is a token function",name);
				if(lli->item.code.sz==0){
					return "postl: [DBG] Empty token list in code_t";
				}
				assert(!prog->buildblock);
				DBGF("'%s' has %d tokens",name,lli->item.code.len);
				const char *errstr=execute_block(prog,lli->item.code);
				if(errstr){
					if(prog->buildblock){
						for(int i=0;i<prog->buildblock->len;i++){
							free(prog->buildblock->tokens[i].str);
						}
						free(prog->buildblock->tokens);
						free(prog->buildblock);
					}
					return errstr;
				}
				assert(!prog->buildblock);
			}
			return NULL;
		}
	}

	/*// Check for a variable
	{
		var_llitem_t *lli=prog->vmap[h];
		while(lli){
			if(strcmp(lli->item.name,name)==0)break;
			lli=lli->next;
		}
		if(lli!=NULL){

		}
	}*/

	// Check for a built-in function
	bool found=false;
	const char *err=execute_builtin(prog,name,&found);
	if(found)return err;

	// Report error
	snprintf(errbuf,256,"postl: function or variable '%s' not found",name);
	return errbuf;
}

void postl_destroy(postl_program_t *prog){
	DBGF("postl_destroy(%p)",prog);

	DBGF("Stack:");
	while(prog->stack){
		stackitem_t *si=prog->stack;
		DBG(
			printf("- type=%s ",valtype_string(si->val.type));
			switch(si->val.type){
				case POSTL_NUM: printf("numv=%g\n",si->val.numv); break;
				case POSTL_STR: printf("strv=%s\n",si->val.strv); break;
				case POSTL_BLOCK: printf("blockv=...\n"); break;
				default: assert(false);
			}
		)
		postl_stackval_release(si->val);
		prog->stack=si->next;
		free(si);
	}

	DBGF("Function map:");
	for(int h=0;h<HASHMAP_SIZE;h++){
		DBG(if(prog->fmap[h])DBGF("- h=%d:",h);)
		while(prog->fmap[h]){
			funcmap_llitem_t *lli=prog->fmap[h];
			DBGF("  - name=%s",lli->item.name);
			funcmap_item_release(lli->item);
			prog->fmap[h]=lli->next;
			free(lli);
		}
	}

	/*for(int h=0;h<HASHMAP_SIZE;h++){
		while(prog->vmap[h]){
			var_llitem_t *lli=prog->vmap[h];
			free(lli->item.name);
			postl_stackval_release(lli->item.val);
			lli=lli->next;
		}
	}*/

	DBGF("buildblock=%p",prog->buildblock);
	if(prog->buildblock){
		token_t *tokens=prog->buildblock->tokens;
		for(int i=0;i<prog->buildblock->len;i++){
			free(tokens[i].str);
		}
		free(tokens);
		free(prog->buildblock);
	}

	while(prog->scopestack){
		for(int h=0;h<HASHMAP_SIZE;h++){
			while(prog->scopestack->vars[h]){
				name_llitem_t *lli=prog->scopestack->vars[h];
				free(lli->name);
				prog->scopestack->vars[h]=lli->next;
				free(lli);
			}
		}
		scope_frame_t *frame=prog->scopestack;
		prog->scopestack=frame->next;
		free(frame);
	}

	free(prog);

	/*DBG(
		DBGF("builtins_hmap:");
		for(int h=0;h<HASHMAP_SIZE;h++){
			if(builtins_hmap[h])DBGF("- h=%d:",h);
			builtin_llitem_t *lli=builtins_hmap[h];
			while(lli){
				DBGF("  - name=%s",lli->name);
				lli=lli->next;
			}
		}
	)*/
}
