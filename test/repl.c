#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <assert.h>
#include <readline/readline.h>
#include <readline/history.h>
#include "../postl.h"

int main(void){
	postl_program_t *prog=postl_makeprogram();
	assert(prog);

	rl_bind_key('\t',rl_complete);
	rl_set_signals();

	char promptbuf[16];

	while(true){
		int ssize=postl_stack_size(prog);
		if(ssize)snprintf(promptbuf,sizeof(promptbuf),"[%d]> ",ssize);
		else strcpy(promptbuf,"> ");

		char *line=readline(promptbuf);
		if(!line)break;
		char *afterspace=line;
		while(isspace(*afterspace))afterspace++;
		if(*afterspace=='\0'){
			free(line);
			continue;
		}
		char *endp=afterspace+strlen(afterspace)-1;
		assert(*endp!='\0');  // should be enforced by strlen
		while(isspace(*endp)&&endp>afterspace)endp--;
		endp++;
		*endp='\0';

		add_history(afterspace);
		const char *errstr=postl_runcode(prog,afterspace);
		free(line);
		if(errstr){
			fprintf(stderr,"\x1B[1m%s\x1B[0m\n",errstr);
		}
	}

	postl_destroy(prog);
}
