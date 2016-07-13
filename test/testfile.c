#include <stdio.h>
#include <stdlib.h>
#include "../postl.h"

// TODO: fix null chars in the file
char* readfile(const char *fname){
	FILE *f=fopen(fname,"rb");
	if(!f)return NULL;
	if(fseek(f,0,SEEK_END)==-1){fclose(f); return NULL;}
	long flen=ftell(f);
	if(flen==-1){fclose(f); return NULL;}
	rewind(f);
	char *buf=malloc(flen+1);
	if(!buf){fclose(f); return NULL;}
	fread(buf,1,flen,f);
	if(ferror(f)){fclose(f); free(buf); return NULL;}
	buf[flen]='\0';
	fclose(f);
	return buf;
}

void func_kaas(postl_program_t *prog){
	if(postl_stack_size(prog)<2){
		printf("func_kaas got %d < 2 arguments\n",postl_stack_size(prog));
		if(postl_stack_size(prog))postl_stack_pop(prog);
		postl_stack_push(prog,postl_stackval_makenum(-1));
		return;
	}
	postl_stackval_t ststr=postl_stack_pop(prog);
	postl_stackval_t stnum=postl_stack_pop(prog);
	printf("func_kaas called with types %d and %d\n",stnum.type,ststr.type);
	printf("func_kaas called with %lf and <%s>\n",stnum.numv,ststr.strv);
	postl_stack_push(prog,postl_stackval_makenum(stnum.numv+1));
	postl_stackval_release(prog,ststr);
	postl_stackval_release(prog,stnum);
}

int main(int argc,char **argv){
	if(argc!=2){
		fprintf(stderr,"Pass postl file as command-line argument\n");
		return 1;
	}
	char *source=readfile(argv[1]);

	const char *errstr;

	postl_program_t *prog=postl_makeprogram();
	postl_register(prog,"kaas",func_kaas);
	if((errstr=postl_addcode(prog,source))){
		fprintf(stderr,"\x1B[31m%s\x1B[0m\n",errstr);
		postl_destroy(prog);
		return 1;
	}

	if((errstr=postl_runglobalcode(prog))){
		fprintf(stderr,"\x1B[31m%s\x1B[0m\n",errstr);
		postl_destroy(prog);
		return 1;
	}

	const postl_stackval_t arglist[]={
		{.type=POSTL_NUM,.numv=6},
		{.type=POSTL_STR,.strv="dingen"}
	};
	postl_stack_pushes(prog,2,arglist);
	if((errstr=postl_callfunction(prog,"main"))){
		fprintf(stderr,"\x1B[31m%s\x1B[0m\n",errstr);
		postl_destroy(prog);
		return 1;
	}

	postl_destroy(prog);
}
