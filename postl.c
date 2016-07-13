#define _GNU_SOURCE  // asprintf
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <assert.h>

#include "postl.h"

#if 0
#define DBG(...) __VA_ARGS__
#else
#define DBG(...)
#endif
#define DBGF(...) DBG(do {fprintf(stderr,__VA_ARGS__); fputc('\n',stderr);} while(0))

#define FUNC_HMAP_SZ (127)

#define GLOBALCODE_FUNCNAME "__global"


__attribute__((noreturn)) static void outofmem(void){
	fprintf(stderr,"postl: OUT OF MEMORY!\n");
	exit(1);
}


static int namehash(const char *name){
	int h=0,len=strlen(name);
	for(int i=0;i<len;i++)h+=name[i]; // apparently works fine
	return h%FUNC_HMAP_SZ;
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


struct postl_program_t{
	stackitem_t *stack;
	int stacksz;
	funcmap_llitem_t *fmap[FUNC_HMAP_SZ];
	code_t *buildblock; //!NULL iff collecting tokens in a { block }
};


static int tokenise(token_t **tokensp,const char *source){
	*tokensp=NULL; //precaution
	int sourcelen=strlen(source);
	int sz=128,len=0;
	token_t *tokens=malloc(sz*sizeof(token_t));
	if(!tokens)outofmem();

#define DESTROY_TOKENS_RET_MIN1 do {for(int i=0;i<len;i++)free(tokens[i].str); free(tokens); return -1;} while(0)

	for(int i=0;i<sourcelen;i++){
		if(source[i]=='#'){ // comment
			i++;
			while(i<sourcelen&&source[i]!='\n')i++;
		} else if(isdigit(source[i])){
			char *endp;
			double nval=strtod(source+i,&endp);
			int numlen=endp-(source+i);
			if(isnan(nval)||isinf(nval)||numlen<=0)DESTROY_TOKENS_RET_MIN1;

			if(len==sz&&(sz*=2,tokens=realloc(tokens,sz))==NULL)outofmem();
			tokens[len].type=TT_NUM;
			tokens[len].str=malloc(numlen+1);
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
			if(j==sourcelen)DESTROY_TOKENS_RET_MIN1;

			if(len==sz&&(sz*=2,tokens=realloc(tokens,sz))==NULL)outofmem();
			tokens[len].type=TT_STR;
			tokens[len].str=malloc(slen+1);
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
						// purposefully skipped: \f, \v
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
					DESTROY_TOKENS_RET_MIN1;
			}
			int j;
			for(j=i+1;j<sourcelen;j++){
				if(!isalpha(source[j])&&source[j]!='_'&&!isdigit(source[j]))break;
			}
			int wordlen=j-i;

			if(len==sz&&(sz*=2,tokens=realloc(tokens,sz))==NULL)outofmem();
			tokens[len].type=isppc?TT_PPC:TT_WORD;
			tokens[len].str=malloc(wordlen+1);
			if(!tokens[len].str)outofmem();
			memcpy(tokens[len].str,source+i,wordlen);
			tokens[len].str[wordlen]='\0';
			len++;
			i=j-1;
		} else if(strchr("+-*/%~&|{}",source[i])!=NULL){
			if(len==sz&&(sz*=2,tokens=realloc(tokens,sz))==NULL)outofmem();
			tokens[len].type=TT_SYMBOL;
			tokens[len].str=malloc(2);
			if(!tokens[len].str)outofmem(); //rlly
			tokens[len].str[0]=source[i];
			tokens[len].str[1]='\0';
			len++;
			i++;
		} else if(strchr(" \t\n\r",source[i])!=NULL){
			//whitespace; pass
		} else DESTROY_TOKENS_RET_MIN1;
	}

#undef DESTROY_TOKENS_RET_MIN1

	*tokensp=tokens;
	return len;
}

static const char* execute_token(postl_program_t *prog,token_t token){
	if(prog->buildblock){
		if(strcmp(token.str,"}")==0){
			stackitem_t *si=malloc(sizeof(stackitem_t));
			if(!si)outofmem();
			si->val.type=POSTL_BLOCK;
			si->val.strv=NULL;
			si->val.blockv=prog->buildblock;
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
		}
		return NULL;
	}
	switch(token.type){
		case TT_NUM:{
			double d=strtod(token.str,NULL);
			stackitem_t *si=malloc(sizeof(stackitem_t));
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
			stackitem_t *si=malloc(sizeof(stackitem_t));
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

typedef enum builtin_enum_t{
	BI_PLUS, BI_MINUS, BI_TIMES, BI_DIVIDE, BI_MODULO,
	BI_PRINT,
	BI_BLOCKOPEN, /*BI_BLOCKCLOSE,*/ //blockclose is directly handled by execute_token
	BI_DEF,
	BI_SWAP, BI_DUP,
	BI_STACKDUMP
} builtin_enum_t;

typedef struct builtin_llitem_t {
	builtin_enum_t id;
	const char *name;
	struct builtin_llitem_t *next;
} builtin_llitem_t;

builtin_llitem_t *builtins_hmap[FUNC_HMAP_SZ]={NULL};
bool builtins_hmap_initialised=false;

static void builtin_add(const char *name,builtin_enum_t id){
	int h=namehash(name);
	builtin_llitem_t *lli=malloc(sizeof(builtin_llitem_t));
	if(!lli)outofmem();
	lli->id=id;
	lli->name=name;
	lli->next=builtins_hmap[h];
	builtins_hmap[h]=lli;
}

static void initialise_builtins_hmap(void){
	builtin_add("+",        BI_PLUS);
	builtin_add("-",        BI_MINUS);
	builtin_add("*",        BI_TIMES);
	builtin_add("/",        BI_DIVIDE);
	builtin_add("%",        BI_MODULO);
	builtin_add("print",    BI_PRINT);
	builtin_add("{",        BI_BLOCKOPEN);
	builtin_add("def",      BI_DEF);
	builtin_add("swap",     BI_SWAP);
	builtin_add("dup",      BI_DUP);
	builtin_add("stackdump",BI_STACKDUMP);

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

#define CANNOT_CONVERT(type,to) \
		RETURN_WITH_ERROR("postl: Cannot convert %s to %s in '%s'",valtype_string((type)),(to),name)

	postl_stackval_t a,b;
	postl_stackval_t res;

	switch(lli->id){
		case BI_PLUS: STACKSIZE_CHECK(2);
			b=postl_stack_pop(prog);
			a=postl_stack_pop(prog);
			if(a.type==POSTL_BLOCK||b.type==POSTL_BLOCK){
				CANNOT_USE(POSTL_BLOCK);
			} else if(a.type==POSTL_STR||b.type==POSTL_STR){
				res.type=POSTL_STR;
				if(a.type!=POSTL_STR){
					if(a.type!=POSTL_NUM)CANNOT_CONVERT(a.type,"string");
					asprintf(&res.strv,"%lf%s",a.numv,b.strv);
					if(!res.strv)outofmem();
				} else {
					if(b.type!=POSTL_NUM)CANNOT_CONVERT(b.type,"string");
					asprintf(&res.strv,"%s%lf",a.strv,b.numv);
					if(!res.strv)outofmem();
				}
			} else {
				assert(a.type==POSTL_NUM&&b.type==POSTL_NUM);
				res.type=POSTL_NUM;
				res.numv=a.numv+b.numv;
			}
			postl_stack_push(prog,res);
			postl_stackval_release(prog,a);
			postl_stackval_release(prog,b);
			break;

#define SIMPLE_BINARY_ARITH_OP(id,expr) \
		case (id): STACKSIZE_CHECK(2); \
			b=postl_stack_pop(prog); \
			a=postl_stack_pop(prog); \
			if(a.type!=POSTL_NUM)CANNOT_USE(a.type); \
			if(b.type!=POSTL_NUM)CANNOT_USE(b.type); \
			res.type=POSTL_NUM; \
			res.numv=(expr); \
			postl_stack_push(prog,res); \
			postl_stackval_release(prog,a); \
			postl_stackval_release(prog,b); \
			break;

		SIMPLE_BINARY_ARITH_OP(BI_MINUS,a.numv-b.numv)
		SIMPLE_BINARY_ARITH_OP(BI_TIMES,a.numv*b.numv)
		SIMPLE_BINARY_ARITH_OP(BI_DIVIDE,b.numv==0?nan(""):a.numv/b.numv)
		SIMPLE_BINARY_ARITH_OP(BI_MODULO,realmodulo(a.numv,b.numv))

#undef SIMPLE_BINARY_ARITH_OP

		case BI_PRINT: STACKSIZE_CHECK(1);
			a=postl_stack_pop(prog);
			switch(a.type){
				case POSTL_NUM:
					printf("%lf",a.numv); fflush(stdout);
					break;
				case POSTL_STR:
					printf("%s",a.strv); fflush(stdout);
					break;
				default:
					CANNOT_USE(a.type);
			}
			postl_stackval_release(prog,a);
			break;

		case BI_BLOCKOPEN:
			assert(!prog->buildblock);
			prog->buildblock=malloc(sizeof(code_t));
			if(!prog->buildblock)outofmem();
			prog->buildblock->sz=128;
			prog->buildblock->len=0;
			prog->buildblock->tokens=malloc(prog->buildblock->sz*sizeof(token_t));
			if(!prog->buildblock->tokens)outofmem();
			break;

		case BI_DEF:{ STACKSIZE_CHECK(2);
			b=postl_stack_pop(prog);
			a=postl_stack_pop(prog);
			if(a.type!=POSTL_STR)
				RETURN_WITH_ERROR("postl: First argument to 'def' should be string, is %s",
					valtype_string(a.type));
			if(b.type!=POSTL_BLOCK)
				RETURN_WITH_ERROR("postl: Second argument to 'def' should be block, is %s",
					valtype_string(b.type));
			int h=namehash(a.strv);
			funcmap_llitem_t *lli=malloc(sizeof(funcmap_llitem_t));
			if(!lli)outofmem();
			lli->item.name=a.strv;
			lli->item.cfunc=NULL;
			lli->item.code=*b.blockv;
			lli->next=prog->fmap[h];
			prog->fmap[h]=lli;
			//postl_stackval_release(prog,a); //values were copied to function definition
			//postl_stackval_release(prog,b);
			break;
		}

		case BI_SWAP:{ STACKSIZE_CHECK(2);
			stackitem_t *bsi=prog->stack;
			prog->stack=prog->stack->next;
			stackitem_t *asi=prog->stack;
			prog->stack=prog->stack->next;
			bsi->next=prog->stack;
			prog->stack=bsi;
			asi->next=prog->stack;
			prog->stack=asi;
			break;
		}

		case BI_DUP: STACKSIZE_CHECK(1);
			postl_stack_push(prog,prog->stack->val);
			break;

		case BI_STACKDUMP:
			for(stackitem_t *si=prog->stack;si;si=si->next){
				switch(si->val.type){
					case POSTL_NUM:
						printf("%lf",si->val.numv);
						break;
					case POSTL_STR:
						pprintstr(si->val.strv);
						break;
					case POSTL_BLOCK:{
						token_t *tokens=si->val.blockv->tokens;
						printf("{ ");
						for(int i=0;i<si->val.blockv->len;i++){
							if(tokens[i].type==TT_STR)pprintstr(tokens[i].str);
							else printf("%s",tokens[i].str);
							putchar(' ');
						}
						putchar('}');
						break;
					}
				}
				if(si->next)printf("  ");
			}
			putchar('\n');
			break;

		default:
			snprintf(errbuf,256,"postl: Sorry, not implemented: builtin '%s'",name);
			return errbuf;
	}

#undef CANNOT_CONVERT
#undef CANNOT_USE
#undef STACKSIZE_CHECK
#undef RETURN_WITH_ERROR
	
	return NULL;
}


postl_program_t* postl_makeprogram(void){
	DBGF("postl_makeprogram()");
	postl_program_t *prog=malloc(sizeof(postl_program_t));
	if(!prog)outofmem();
	prog->stack=NULL;
	prog->stacksz=0;
	for(int i=0;i<FUNC_HMAP_SZ;i++){
		prog->fmap[i]=NULL;
	}

	int h=namehash(GLOBALCODE_FUNCNAME);
	prog->fmap[h]=malloc(sizeof(funcmap_llitem_t));
	if(!prog->fmap[h])outofmem();

	prog->fmap[h]->next=NULL;

	int len=strlen(GLOBALCODE_FUNCNAME);
	funcmap_item_t *item=&prog->fmap[h]->item;
	item->name=malloc(len+1);
	if(!item->name)outofmem();
	memcpy(item->name,GLOBALCODE_FUNCNAME,len+1);

	item->cfunc=NULL;

	item->code.sz=128;
	item->code.len=0;
	item->code.tokens=malloc(item->code.sz*sizeof(token_t));
	if(!item->code.tokens)outofmem();

	prog->buildblock=NULL;

	initialise_builtins_hmap();

	return prog;
}

void postl_register(postl_program_t *prog,const char *name,void (*func)(postl_program_t*)){
	DBGF("postl_register(%p,%s,%p)",prog,name,func);
	int h=namehash(name);
	int len=strlen(name);
	funcmap_llitem_t *llitem=malloc(sizeof(funcmap_llitem_t));
	if(!llitem)outofmem();
	llitem->item.name=malloc(len+1);
	if(!llitem->item.name)outofmem();
	memcpy(llitem->item.name,name,len+1);
	llitem->item.cfunc=func;
	llitem->item.code.sz=0;
	llitem->item.code.len=0;
	llitem->item.code.tokens=NULL;
	llitem->next=prog->fmap[h];
	prog->fmap[h]=llitem;
}

const char* postl_addcode(postl_program_t *prog,const char *source){
	DBGF("postl_addcode(%p,<<<\"%s\">>>)",prog,source);
	token_t *tokens=NULL;
	int len=tokenise(&tokens,source);
	if(len<0)return "Tokenise error";
	DBG(
		DBGF("%d tokens parsed",len);
		for(int i=0;i<len;i++){
			DBGF("token: type=%d '%s'",tokens[i].type,tokens[i].str);
		}
	)
	assert(tokens);
	code_t *code=&prog->fmap[namehash(GLOBALCODE_FUNCNAME)]->item.code;
	if(code->len+len>code->sz){
		code->sz=code->len+len+16;
		code->tokens=realloc(code->tokens,code->sz);
		if(!code->tokens)outofmem();
	}
	memcpy(code->tokens+code->len,tokens,len*sizeof(token_t));
	code->len+=len;
	free(tokens);
	return NULL;
}

const char* postl_runglobalcode(postl_program_t *prog){
	DBGF("postl_runglobalcode(%p)",prog);
	return postl_callfunction(prog,GLOBALCODE_FUNCNAME);
}

postl_stackval_t postl_stackval_makenum(int num){
	DBGF("postl_stackval_makenum(%d)",num);
	postl_stackval_t st={.type=POSTL_NUM,.numv=num};
	return st;
}

postl_stackval_t postl_stackval_makestr(const char *str){
	DBGF("postl_stackval_makestr(%s)",str);
	int len=strlen(str);
	postl_stackval_t st={.type=POSTL_STR,.strv=malloc(len+1)};
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
	stackitem_t *si=malloc(sizeof(stackitem_t));
	if(!si)outofmem();
	si->val.type=val.type;
	si->val.numv=val.numv;
	if(val.type==POSTL_STR){
		if(val.strv==NULL){
			fprintf(stderr,"postl: NULL string in stack value to postl_stack_push\n");
			exit(1);
		}
		int len=strlen(val.strv);
		si->val.strv=malloc(len+1);
		if(!si->val.strv)outofmem();
		memcpy(si->val.strv,val.strv,len+1);
	} else si->val.strv=NULL;
	if(val.type==POSTL_BLOCK){
		if(val.blockv==NULL){
			fprintf(stderr,"postl: NULL block in stack value to postl_stack_push\n");
			exit(1);
		}
		code_t *code=malloc(sizeof(code_t));
		if(!code)outofmem();
		code->sz=val.blockv->sz;
		code->len=val.blockv->len;
		code->tokens=malloc(code->sz*sizeof(token_t));
		if(!code->tokens)outofmem();
		for(int i=0;i<code->len;i++){
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

void postl_stackval_release(postl_program_t *prog,postl_stackval_t val){
	(void)prog;
	DBGF("postl_stackval_release(%p,{type=%d,...})",prog,val.type);
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

	// Check for a user-defined function
	int h=namehash(name);
	funcmap_llitem_t *lli=prog->fmap[h];
	while(lli){
		if(strcmp(lli->item.name,name)==0)break;
		lli=lli->next;
	}
	if(lli!=NULL){
		DBGF("Calling '%s' -> user-defined function...",name);
		if(lli->item.cfunc)lli->item.cfunc(prog);
		else {
			DBGF("'%s' is token function",name);
			if(lli->item.code.sz==0){
				return "postl: [DBG] Empty token list in code_t";
			}
			token_t *tokens=lli->item.code.tokens;
			int codelen=lli->item.code.len;
			assert(!prog->buildblock);
			DBGF("'%s' has code length %d",name,codelen);
			for(int i=0;i<codelen;i++){
				const char *errstr=execute_token(prog,tokens[i]);
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
			}
			if(prog->buildblock)return "postl: Unclosed block in source";
		}
		return NULL;
	}

	// Check for a variable (first need to implement variables)

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
	while(prog->stack){
		stackitem_t *si=prog->stack;
		if(si->val.strv)free(si->val.strv);
		prog->stack=si->next;
		free(si);
	}
	for(int h=0;h<FUNC_HMAP_SZ;h++){
		while(prog->fmap[h]){
			funcmap_llitem_t *lli=prog->fmap[h];
			free(lli->item.name);
			if(lli->item.code.sz!=0){
				token_t *tokens=lli->item.code.tokens;
				for(int i=0;i<lli->item.code.len;i++){
					free(tokens[i].str);
				}
				free(tokens);
			}
			prog->fmap[h]=lli->next;
			free(lli);
		}
	}
	if(prog->buildblock){
		token_t *tokens=prog->buildblock->tokens;
		for(int i=0;i<prog->buildblock->len;i++){
			free(tokens[i].str);
		}
		free(tokens);
		free(prog->buildblock);
	}
	free(prog);
}
