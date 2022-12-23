#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <fcntl.h>

#define MAX_LENGTH 2048
//macro to flush stdout when printing
#define printf(...) printf(__VA_ARGS__);fflush(stdout)

static inline void set_signal(int signal, void* sig_handler){
	struct sigaction handler = {0};
	handler.sa_handler = sig_handler;
	sigfillset(&handler.sa_mask);
	handler.sa_flags = 0;
	sigaction(signal, &handler, NULL);
}
bool force_foreground=false;
void sig_stp(int signo){
	force_foreground=!force_foreground;
	char* message=force_foreground?"Entering foreground-only mode (& is now ignored)\n":"Exiting foreground-only mode\n";
	int len=force_foreground?49:29;
	write(STDOUT_FILENO, message, len);
}
//split string into array of strings, allocated as one chunk, starting with pointers then the string,
//modifiled to have an extra null pointer for execvp
char** split_string_by_char(char* str, char seperator, int *section_count){
	int n_index=0,n_count=1;
	for(char* s=str;*s;s++)
		if(*s==seperator)
			n_count++;

	char** sections=calloc((n_count+1)*sizeof(char*)+strlen(str)+1,1);
	char* start = ((char*)sections)+((n_count+1)*sizeof(char*));

	sections[n_count]=NULL;
	*section_count=n_count;
	sections[0]=start;

	strcpy(start,str);
	for(char* s=start;*s;s++){
		if(*s==seperator){
			*s=0;
			sections[++n_index]=s+1;
		}
	}
	return sections;
}
void handle_file_redirection(char** argv, int argc, bool background){
	if(background){
		int fd = open("/dev/null", O_RDONLY);
		if (dup2(fd, STDIN_FILENO) == -1) { fprintf(stderr, "Error redirecting stdin to null"); exit(1); };
		close(fd);
		fd = open("/dev/null", O_WRONLY);
		if (dup2(fd, STDOUT_FILENO) == -1) { fprintf(stderr, "Error redirecting stdout to null"); exit(1); };
		close(fd);
	}
	for (int i = 0; i < argc; ++i) {
		if(!strcmp(argv[i],"<")|!strcmp(argv[i],">")){
			if(argv[i][0]=='<'){
				int fd=open(argv[i+1], O_RDONLY);
				if (fd==-1) { fprintf(stderr, "Error cannot open file: %s for input\n", argv[i+1]); exit(1); }
				if (dup2(fd, STDIN_FILENO) == -1){
					fprintf(stderr, "Error redirecting stdin"); exit(1);
				}
				close(fd);
				memmove(argv+i,argv+i+2,(argc-i-2)*sizeof(char*));
				argv[--argc]=NULL;argv[--argc]=NULL;
				i-=1;
			}
			else if(argv[i][0]=='>'){
				int fd = open(argv[i+1], O_CREAT | O_RDWR | O_TRUNC, 0644);
				if (fd==-1) { fprintf(stderr, "Cannot open %s for ouput\n", argv[i+1]); exit(1); }
				if (dup2(fd, STDOUT_FILENO) == -1){
					fprintf(stderr, "Error redirecting"); exit(1);
				}
				close(fd);
				memmove(argv+i,argv+i+2,(argc-i-2)*sizeof(char*));
				argv[--argc]=NULL;argv[--argc]=NULL;
				i-=1;
			}
		}
	}
}
void check_background(){
	int pid,status_code;
	do { //check for zombies
		pid = waitpid(-1, &status_code, WNOHANG);
		if (pid > 0) {
			if (WIFEXITED(status_code) != 0) {
				printf("background pid %d is done: exit value %d\n", pid, WEXITSTATUS(status_code));
			}
			else if (WIFSIGNALED(status_code) != 0) {
				printf("background pid %d is done: terminated by signal %d\n", pid, WTERMSIG(status_code));
			}
		}
	} while (pid > 0); // Loop stops once pid returns 0 (no child process has terminated).
}
bool run_cmd(char* cmd){
	int argc;
	char** argv=split_string_by_char(cmd,' ',&argc);
	static int status;
	bool background=(strcmp(argv[argc-1],"&")==0);
	if(background)
		argv[--argc]=NULL;
	background=background&!force_foreground;
	//handle_file_redirection(argv, argc, background);
	if(!strcmp(argv[0],"cd")){
		if(argc>1){
			if(chdir(argv[1])==-1){
				printf("sh: cd: %s: No such file or directory",argv[1]);
			}
		}
		else{
			chdir(getenv("HOME"));
		}
	}
	else if(!strcmp(argv[0],"exit")){
		free(argv);
		return false;
	}
	else if(!strcmp(argv[0],"status")){
		if (WIFEXITED(status))	{// exited fine
			printf("exit value %d\n", WEXITSTATUS(status));
		}
		else if (WIFSIGNALED(status)){//rip, signaled
			printf("terminated by signal %d\n", WTERMSIG(status));
		}
	}
	else {// Time to fork a child
		pid_t pid = fork();
		if (pid == -1){
			fprintf(stderr, "Hull Breach!!\n");
			exit(1);
		}
		if (pid == 0){	// Child process
			if (!background) {    // child processes ran on foreground can be terminated
				set_signal(SIGINT, SIG_DFL);
			}
			handle_file_redirection(argv, argc, background);
			execvp(argv[0], argv);
			fprintf(stderr, "%s: command not found\n", argv[0]);
			fflush(stderr);
			exit(1);
		}
		else { // Parent process
			if (background){
				printf("background pid is %d\n", pid);
			}
			else {// Wait for child to catch up
				waitpid(pid, &status, 0);
				if (WTERMSIG(status) != 0){// Check if child process was killed by signal
					printf("terminated by signal %d\n", WTERMSIG(status));
				}
			}
		}
	}
	free(argv);
	return true;
}
void get_cmd(char* final_cmd){
	char cmd[MAX_LENGTH*2]={0};
	while(strcmp(cmd,"")==0||cmd[0]=='#'){
		check_background();
		printf(":");
		fflush(stdout);
		fgets(cmd,MAX_LENGTH,stdin);
		cmd[strcspn(cmd, "\n")] = 0;
	}
	int pid=getpid();
	char* fin=final_cmd,*c=cmd,*s;
	//whlie still pids to replace
	while((s=strstr(c,"$$"))){
		//copy text till then
		memcpy(fin,c,s-c);
		//increment dest pointer
		fin+=s-c;
		//move past $$
		c=s+2;
		//add pid
		fin+=sprintf(fin,"%i",pid);
	}
	//copy remaining
	strcpy(fin, c);
}
int main(){
	set_signal(SIGINT,SIG_IGN);
	set_signal(SIGTSTP, sig_stp);

	_Bool running=1;
	while(running){
		char cmd[MAX_LENGTH*2]={0};
		get_cmd(cmd);

		running=run_cmd(cmd);
	}
	return 0;
}