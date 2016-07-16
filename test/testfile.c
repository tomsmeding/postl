#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
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

char *readstdin(void){
	int bufsz=1024,cursor=0;
	char *buf=malloc(bufsz);
	if(!buf)return NULL;
	while(true){
		if(cursor==bufsz-1){
			bufsz*=2;
			char *newbuf=realloc(buf,bufsz);
			if(!newbuf){
				free(buf);
				return NULL;
			}
			buf=newbuf;
		}
		int nread=fread(buf,1,bufsz-cursor-1,stdin);
		cursor+=nread;
		if(nread<bufsz-cursor-1){
			if(feof(stdin))break;
			if(ferror(stdin)){
				free(buf);
				return NULL;
			}
		}
	}
	buf[cursor]='\0';
	return buf;
}

void func_kaas(postl_program_t *prog){
	if(postl_stack_size(prog)<2){
		fprintf(stderr,"func_kaas got %d < 2 arguments\n",postl_stack_size(prog));
		postl_destroy(prog);
		exit(1);
	}
	postl_stackval_t ststr=postl_stack_pop(prog);
	postl_stackval_t stnum=postl_stack_pop(prog);
	printf("func_kaas called with types %d and %d\n",stnum.type,ststr.type);
	printf("func_kaas called with %lf and <%s>\n",stnum.numv,ststr.strv);
	postl_stack_push(prog,postl_stackval_makenum(stnum.numv+1));
	postl_stackval_release(ststr);
	postl_stackval_release(stnum);
}

int main(int argc,char **argv){
	if(argc!=2){
		fprintf(stderr,"Pass postl file as command-line argument\n");
		return 1;
	}
	char *source;
	if(strcmp(argv[1],"-")==0){
		source=readstdin();
		if(!source){
			fprintf(stderr,"Cannot read from stdin\n");
			return 1;
		}
	} else {
		source=readfile(argv[1]);
		if(!source){
			fprintf(stderr,"Cannot read file '%s'\n",argv[1]);
			return 1;
		}
	}

	const char *errstr;

	postl_program_t *prog=postl_makeprogram();
	postl_register(prog,"kaas",func_kaas);
	if((errstr=postl_runcode(prog,source))){
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
