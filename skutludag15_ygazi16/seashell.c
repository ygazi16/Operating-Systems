#include <limits.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>            //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
const char *sysname = "seashell";

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_RESET   "\x1b[0m"

#define SIZE 516

enum return_codes {
    SUCCESS = 0,
    EXIT = 1,
    UNKNOWN = 2,
};

struct command_t {
    char *name;
    bool background;
    bool auto_complete;
    int arg_count;
    char **args;
    char *redirects[3]; // in/out redirection
    struct command_t *next; // for piping
};

struct Queue {
    int front, rear, size;
    unsigned capacity;
    char** array;
};


struct Queue* createQueue(unsigned capacity)
{
    struct Queue* queue = (struct Queue*)malloc(
        sizeof(struct Queue));
    queue->capacity = capacity;
    queue->front = queue->size = 0;
 
    // This is important, see the enqueue
    queue->rear = capacity - 1;
    queue->array = (char**)malloc(
        queue->capacity * sizeof(char*));
    return queue;
}


int isFull(struct Queue* queue)
{
    return (queue->size == queue->capacity);
}

int isEmpty(struct Queue* queue)
{
    return (queue->size == 0);
}

void enqueue(struct Queue* queue, char *item)
{
    if (isFull(queue))
        return;
    queue->rear = (queue->rear + 1)
                  % queue->capacity;
    queue->array[queue->rear] = item;
    queue->size = queue->size + 1;
    printf("%s enqueued to queue\n", item);
}

char* dequeue(struct Queue* queue)
{
    if (isEmpty(queue))
        return "*";
    char* item = queue->array[queue->front];
    queue->front = (queue->front + 1)
                   % queue->capacity;
    queue->size = queue->size - 1;
    return item;
}

char* front(struct Queue* queue)
{
    if (isEmpty(queue))
        return "*";
    return queue->array[queue->front];
}

char* rear(struct Queue* queue)
{
    if (isEmpty(queue))
        return "*";
    return queue->array[queue->rear];
}

/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t *command) {
    int i = 0;
    printf("Command: <%s>\n", command->name);
    printf("\tIs Background: %s\n", command->background ? "yes" : "no");
    printf("\tNeeds Auto-complete: %s\n", command->auto_complete ? "yes" : "no");
    printf("\tRedirects:\n");
    for (i = 0; i < 3; i++)
        printf("\t\t%d: %s\n", i, command->redirects[i] ? command->redirects[i] : "N/A");
    printf("\tArguments (%d):\n", command->arg_count);
    for (i = 0; i < command->arg_count; ++i)
        printf("\t\tArg %d: %s\n", i, command->args[i]);
    if (command->next) {
        printf("\tPiped to:\n");
        print_command(command->next);
    }


}

/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command) {
    if (command->arg_count) {
        for (int i = 0; i < command->arg_count; ++i)
            free(command->args[i]);
        free(command->args);
    }
    for (int i = 0; i < 3; ++i)
        if (command->redirects[i])
            free(command->redirects[i]);
    if (command->next) {
        free_command(command->next);
        command->next = NULL;
    }
    free(command->name);
    free(command);
    return 0;
}

/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt() {
    char cwd[1024], hostname[1024];
    gethostname(hostname, sizeof(hostname));
    getcwd(cwd, sizeof(cwd));
    printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
    return 0;
}

/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command) {
    const char *splitters = " \t"; // split at whitespace
    int index, len;
    len = strlen(buf);
    while (len > 0 && strchr(splitters, buf[0]) != NULL) // trim left whitespace
    {
        buf++;
        len--;
    }
    while (len > 0 && strchr(splitters, buf[len - 1]) != NULL)
        buf[--len] = 0; // trim right whitespace

    if (len > 0 && buf[len - 1] == '?') // auto-complete
        command->auto_complete = true;
    if (len > 0 && buf[len - 1] == '&') // background
        command->background = true;

    char *pch = strtok(buf, splitters);
    command->name = (char *) malloc(strlen(pch) + 1);
    if (pch == NULL)
        command->name[0] = 0;
    else
        strcpy(command->name, pch);

    command->args = (char **) malloc(sizeof(char *));

    int redirect_index;
    int arg_index = 0;
    char temp_buf[1024], *arg;
    while (1) {
        // tokenize input on splitters
        pch = strtok(NULL, splitters);
        if (!pch) break;
        arg = temp_buf;
        strcpy(arg, pch);
        len = strlen(arg);

        if (len == 0) continue; // empty arg, go for next
        while (len > 0 && strchr(splitters, arg[0]) != NULL) // trim left whitespace
        {
            arg++;
            len--;
        }
        while (len > 0 && strchr(splitters, arg[len - 1]) != NULL) arg[--len] = 0; // trim right whitespace
        if (len == 0) continue; // empty arg, go for next

        // piping to another command
        if (strcmp(arg, "|") == 0) {
            struct command_t *c = malloc(sizeof(struct command_t));
            int l = strlen(pch);
            pch[l] = splitters[0]; // restore strtok termination
            index = 1;
            while (pch[index] == ' ' || pch[index] == '\t') index++; // skip whitespaces

            parse_command(pch + index, c);
            pch[l] = 0; // put back strtok termination
            command->next = c;
            continue;
        }

        // background process
        if (strcmp(arg, "&") == 0)
            continue; // handled before

        // handle input redirection
        redirect_index = -1;
        if (arg[0] == '<')
            redirect_index = 0;
        if (arg[0] == '>') {
            if (len > 1 && arg[1] == '>') {
                redirect_index = 2;
                arg++;
                len--;
            } else redirect_index = 1;
        }
        if (redirect_index != -1) {
            command->redirects[redirect_index] = malloc(len);
            strcpy(command->redirects[redirect_index], arg + 1);
            continue;
        }

        // normal arguments
        if (len > 2 && ((arg[0] == '"' && arg[len - 1] == '"')
                        || (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
        {
            arg[--len] = 0;
            arg++;
        }
        command->args = (char **) realloc(command->args, sizeof(char *) * (arg_index + 1));
        command->args[arg_index] = (char *) malloc(len + 1);
        strcpy(command->args[arg_index++], arg);
    }
    command->arg_count = arg_index;
    return 0;
}

void prompt_backspace() {
    putchar(8); // go back 1
    putchar(' '); // write empty over
    putchar(8); // go back 1 again
}

/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command) {
    int index = 0;
    char c;
    char buf[4096];
    static char oldbuf[4096];

    // tcgetattr gets the parameters of the current terminal
    // STDIN_FILENO will tell tcgetattr that it should write the settings
    // of stdin to oldt
    static struct termios backup_termios, new_termios;
    tcgetattr(STDIN_FILENO, &backup_termios);
    new_termios = backup_termios;
    // ICANON normally takes care that one line at a time will be processed
    // that means it will return if it sees a "\n" or an EOF or an EOL
    new_termios.c_lflag &= ~(ICANON | ECHO); // Also disable automatic echo. We manually echo each char.
    // Those new settings will be set to STDIN
    // TCSANOW tells tcsetattr to change attributes immediately.
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);


    //FIXME: backspace is applied before printing chars
    show_prompt();
    int multicode_state = 0;
    buf[0] = 0;
    while (1) {
        c = getchar();
        // printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

        if (c == 9) // handle tab
        {
            buf[index++] = '?'; // autocomplete
            break;
        }

        if (c == 127) // handle backspace
        {
            if (index > 0) {
                prompt_backspace();
                index--;
            }
            continue;
        }
        if (c == 27 && multicode_state == 0) // handle multi-code keys
        {
            multicode_state = 1;
            continue;
        }
        if (c == 91 && multicode_state == 1) {
            multicode_state = 2;
            continue;
        }
        if (c == 65 && multicode_state == 2) // up arrow
        {
            int i;
            while (index > 0) {
                prompt_backspace();
                index--;
            }
            for (i = 0; oldbuf[i]; ++i) {
                putchar(oldbuf[i]);
                buf[i] = oldbuf[i];
            }
            index = i;
            continue;
        } else
            multicode_state = 0;

        putchar(c); // echo the character
        buf[index++] = c;
        if (index >= sizeof(buf) - 1) break;
        if (c == '\n') // enter key
            break;
        if (c == 4) // Ctrl+D
            return EXIT;
    }
    if (index > 0 && buf[index - 1] == '\n') // trim newline from the end
        index--;
    buf[index++] = 0; // null terminate string

    strcpy(oldbuf, buf);

    parse_command(buf, command);

    // print_command(command); // DEBUG: uncomment for debugging

    // restore the old settings
    tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
    return SUCCESS;
}

int process_command(struct command_t *command);

int main() {
    while (1) {
        struct command_t *command = malloc(sizeof(struct command_t));
        memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

        int code;
        code = prompt(command);
        if (code == EXIT) break;

        code = process_command(command);
        if (code == EXIT) break;

        free_command(command);
    }

    printf("\n");
    return 0;
}

void red () {
  printf("\033[1;31m");
}
void blue () {
  printf("\033[0;34m");
}
void green () {
  printf("\033[0;32m");
}
void reset () {
  printf("\033[0m");
}

void checkHostName(int hostname)
{
    if (hostname == -1)
    {
        perror("gethostname");
        exit(1);
    }
}
  
// Returns host information corresponding to host name
void checkHostEntry(struct hostent * hostentry)
{
    if (hostentry == NULL)
    {
        perror("gethostbyname");
        exit(1);
    }
}
  
// Converts space-delimited IPv4 addresses
// to dotted-decimal format
void checkIPbuffer(char *IPbuffer)
{
    if (NULL == IPbuffer)
    {
        perror("inet_ntoa");
        exit(1);
    }
}

int process_command(struct command_t *command) {

    int r;
    if (strcmp(command->name, "") == 0) return SUCCESS;

    if (strcmp(command->name, "exit") == 0)
        return EXIT;

    if (strcmp(command->name, "cd") == 0) {
        if (command->arg_count > 0) {
            r = chdir(command->args[0]);
            if (r == -1)
                printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
            return SUCCESS;
        }
    }

    if (strcmp(command->name, "shortdir") == 0) {

        FILE *fptr = NULL;
        FILE *fptrTemp = NULL;
        remove("/home/abrakadabra/Desktop/LastTerm/COMP304/Project1/filesTemporary.txt");
        fptrTemp = fopen("/home/abrakadabra/Desktop/LastTerm/COMP304/Project1/filesTemporary.txt", "a+");

        if (strcmp(command->args[0], "set") == 0) {

        	fptr = fopen("/home/abrakadabra/Desktop/LastTerm/COMP304/Project1/files.txt", "a+");
        	char name[10];
            char location[100];
            while (fscanf(fptr, "%s %s", name, location) != EOF) {
                if (strcmp(name, command->args[1]) == 0) {


                } else {

                    fprintf(fptrTemp, "%s %s\n", name, location);

                }
            }
            remove("/home/abrakadabra/Desktop/LastTerm/COMP304/Project1/files.txt");
            fclose(fptr);
            fclose(fptrTemp);
            fptr = fopen("/home/abrakadabra/Desktop/LastTerm/COMP304/Project1/files.txt", "a+");
            fptrTemp = fopen("/home/abrakadabra/Desktop/LastTerm/COMP304/Project1/filesTemporary.txt", "a+");

            while (fscanf(fptrTemp, "%s %s", name, location) != EOF) {

                fprintf(fptr, "%s %s\n", name, location);
            }
            char *dir = getcwd(NULL, 0);
            fprintf(fptr, "%s %s\n", command->args[1], dir);

           
            fclose(fptrTemp);
            fptrTemp = NULL;

        } else if (strcmp(command->args[0], "del") == 0) {

        	fptr = fopen("/home/abrakadabra/Desktop/LastTerm/COMP304/Project1/files.txt", "a+");
            char name[10];
            char location[100];
            while (fscanf(fptr, "%s %s", name, location) != EOF) {
                if (strcmp(name, command->args[1]) == 0) {


                } else {

                    fprintf(fptrTemp, "%s %s\n", name, location);

                }
            }
            remove("/home/abrakadabra/Desktop/LastTerm/COMP304/Project1/files.txt");
            fclose(fptr);
            fclose(fptrTemp);
            fptr = fopen("/home/abrakadabra/Desktop/LastTerm/COMP304/Project1/files.txt", "a+");
            fptrTemp = fopen("/home/abrakadabra/Desktop/LastTerm/COMP304/Project1/filesTemporary.txt", "a+");

            while (fscanf(fptrTemp, "%s %s", name, location) != EOF) {

                fprintf(fptr, "%s %s\n", name, location);
            }

            fclose(fptrTemp);
            fptrTemp = NULL;

        } else if (strcmp(command->args[0], "clear") == 0) {

            remove("/home/abrakadabra/Desktop/LastTerm/COMP304/Project1/files.txt");
            fptr = fopen("/home/abrakadabra/Desktop/LastTerm/COMP304/Project1/files.txt", "a+");

        } else if (strcmp(command->args[0], "list") == 0) {

        	fptr = fopen("/home/abrakadabra/Desktop/LastTerm/COMP304/Project1/files.txt", "a+");
            char name[10];
            char location[100];
            while (fscanf(fptr, "%s %s", name, location) != EOF) {

                printf("Name-Directory: %s %s\n", name, location);

            }


        } else if (strcmp(command->args[0], "jump") == 0) {

        	fptr = fopen("/home/abrakadabra/Desktop/LastTerm/COMP304/Project1/files.txt", "a+");
            char name[10];
            char location[100];

            while (fscanf(fptr, "%s %s", name, location) != EOF) {
                if (strcmp(name, command->args[1]) == 0) {

                    r = chdir(location);
                    if (r == -1)  printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));

                }
            }
        }

        fclose(fptr);
        fptr = NULL;
        return SUCCESS;

    }
    if(strcmp(command->name, "myNetwork") == 0){

   		char hostbuffer[256];
    	char *IPbuffer;
    	struct hostent *host_entry;
    	int hostname;
  
    // To retrieve hostname
    	hostname = gethostname(hostbuffer, sizeof(hostbuffer));
    	checkHostName(hostname);
  
    // To retrieve host information
 	   	host_entry = gethostbyname(hostbuffer);
    	checkHostEntry(host_entry);
  
    // To convert an Internet network
    // address into ASCII string
 	   	IPbuffer = inet_ntoa(*((struct in_addr*) host_entry->h_addr_list[0]));
  
    	printf("Hostname: %s\n", hostbuffer);
	    printf("Host IP: %s\n", IPbuffer);
  


    }
    if(strcmp(command->name, "goodMorning") == 0){


    	char *time = command->args[0];
		char *path = command->args[1];
		char *token;
		

		const char s[2] = ".";
		token = strtok(time,s);
		char *hour = token;
		token = strtok(NULL,s); 
		char *minute = token;
		FILE *fptr = NULL;
		remove("morning.txt");
        fptr = fopen("morning.txt", "a+");
        fprintf(fptr, "%s %s * * * env DISPLAY=:0.0 audacious /home/abrakadabra/deneme %s\n", minute, hour,path);
        fclose(fptr);
        fptr = NULL;
        
        pid_t pid = fork();
    	if (pid == 0) {
        /// This shows how to do exec with environ (but is not available on MacOs)
        // extern char** environ; // environment variables
        // execvpe(command->name, command->args, environ); // exec+args+path+environ

        /// This shows how to do exec with auto-path resolve
        // add a NULL argument to the end of args, and the name to the beginning
        // as required by exec

        // increase args size by 2
        	execlp("crontab", "crontab", "morning.txt",NULL); // exec+args+path
        	exit(0);
        /// TODO: do your own exec  path resolving using execv()
    	} else {

    		sleep(1);
        	wait(0); 
       // wait for child process to finish
        	return SUCCESS;
    	}    	
    }
    if(strcmp(command->name, "highlight") == 0){

        char read_el[SIZE];
        FILE *fp=fopen(command->args[2], "r");
        char *token;
        char *f1 = fgets(read_el,SIZE,fp);

        if(fp == NULL){
            printf("File Opening Error!!");

        }
        const char s[4] = " ,.";
        while (f1 != NULL){
        	
        	token = strtok(read_el,s); 

        	while(token != NULL){

        		if(strcmp(token,command->args[0]) == 0){

        			if(strstr(command->args[1],"r") != NULL){
             
                		red();
                		printf("%s ", token);
                		reset();

                	}else if(strstr(command->args[1],"g") != NULL){

                		green();
                		printf("%s ", token);
                		reset();

                	}else if(strstr(command->args[1],"b") != NULL){

	            		blue();
	            		printf("%s ", token);
                		reset();

                	}else{

                    	printf("%s ", token);
                	}


        		}else{

        			printf("%s ", token);
        		}


        		token = strtok(NULL,s); 

        	}
        	f1 = fgets(read_el,SIZE,fp);
        }

        fclose(fp);
        fp = NULL;
        return SUCCESS;
    }

    if(strcmp(command->name, "kdiff") == 0){

    	char read_el1[SIZE];
    	char read_el2[SIZE];

    	if(strcmp(command->args[0],"-a") == 0){

    		if(strstr(command->args[1],".txt") == NULL || strstr(command->args[2],".txt") == NULL){

    			printf("Invalid Text Names\n");

    		}else{

    			int valid = 0;
    			int difference = 0;
        		FILE *fptr1 = fopen(command->args[1], "r");
        		FILE *fptr2 = fopen(command->args[2], "r");

        		char *f1 = fgets(read_el1,SIZE,fptr1);
        		char *f2 = fgets(read_el2,SIZE,fptr2);

        		int line = 1;

        		while(f1 != NULL && f2 != NULL){

					if(strcmp(read_el1,read_el2) == 0){
				
					}else{

						valid = 1;
						printf("Line%d for 1st Text: %s\n", line, read_el1);
						printf("Line%d for 2nd Text: %s\n", line, read_el2);
						difference = difference + 1;

					}
					
					f1 = fgets(read_el1,SIZE,fptr1);
	        		f2 = fgets(read_el2,SIZE,fptr2);
	        		line = line + 1;

        		}
    	    	while(true){
	        		if(f1 != NULL){

						printf("Line%d for 1st Text Alone: %s\n", line, read_el1);
						f1 = fgets(read_el1,SIZE,fptr1);
	        			line = line + 1;
	        			difference = difference + 1;

        			}else if(f2 != NULL){

        				printf("Line%d for 2nd Text Alone: %s\n", line, read_el2);
	        			f2 = fgets(read_el2,SIZE,fptr2);
	        			line = line + 1;
	        			difference = difference + 1;

        			}else{
        				break;
        			}
        		}

        		if(valid == 0){

        			printf("The two files are identical\n");

        		}else{

        			printf("%d different lines are found\n", difference);
        		}

        		fclose(fptr2);
        		fptr2 = NULL;
        		fclose(fptr1);
        		fptr1 = NULL;
        	}

    	}else if(strcmp(command->args[0],"-b") == 0){

        	FILE *fptr1 = fopen(command->args[1], "rb");
        	FILE *fptr2 = fopen(command->args[2], "rb");

        	int f1 = fgetc(fptr1);
       		int f2 = fgetc(fptr2);
		
			int count = 0;

			while(f1 != EOF && f2 != EOF){

				if(f1 == f2){

				}else{

					count = count + 1;

				}

				f1 = fgetc(fptr1);
				f2 = fgetc(fptr2);

			}

			while(true){

				if(f1 != EOF){

					count = count + 1;
					f1 = fgetc(fptr1);
				}
				if(f2 != EOF){

					count = count + 1;
					f2 = fgetc(fptr2);

				}else{

					break;
				
				}
			}

			if(count == 0){

				printf("The two files are identical.\n");

			}else{

				printf("The two files are different in %d bytes\n",count);

			}
			fclose(fptr2);
        	fptr2 = NULL;
        	fclose(fptr1);
        	fptr1 = NULL;

    	}else{
			
			if(strstr(command->args[1],".txt") == NULL || strstr(command->args[2],".txt") == NULL){

    			printf("Invalid Text Names\n");

    		}else{

    			int valid = 0;
    			int difference = 0;
        		FILE *fptr1 = fopen(command->args[0], "r");
        		FILE *fptr2 = fopen(command->args[1], "r");

        		char *f1 = fgets(read_el1,SIZE,fptr1);
        		char *f2 = fgets(read_el2,SIZE,fptr2);

        		int line = 1;

        		while(f1 != NULL && f2 != NULL){

					if(strcmp(read_el1,read_el2) == 0){
				
					}else{

						valid = 1;
						printf("Line%d for 1st Text: %s\n", line, read_el1);
						printf("Line%d for 2nd Text: %s\n", line, read_el2);
						difference = difference + 1;

					}
					
					f1 = fgets(read_el1,SIZE,fptr1);
	        		f2 = fgets(read_el2,SIZE,fptr2);
	        		line = line + 1;

        		}
    	    	while(true){
	        		if(f1 != NULL){

						printf("Line%d for 1st Text Alone: %s\n", line, read_el1);
						f1 = fgets(read_el1,SIZE,fptr1);
	        			line = line + 1;
	        			difference = difference + 1;

        			}else if(f2 != NULL){

        				printf("Line%d for 2nd Text Alone: %s\n", line, read_el2);
	        			f2 = fgets(read_el2,SIZE,fptr2);
	        			line = line + 1;
	        			difference = difference + 1;

        			}else{
        				break;
        			}
        		}

        		if(valid == 0){

        			printf("The two files are identical\n");

        		}else{

        			printf("%d different lines are found\n", difference);
        		}

        		fclose(fptr2);
        		fptr2 = NULL;
        		fclose(fptr1);
        		fptr1 = NULL;
        	}
    	}
        
        return SUCCESS;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /// This shows how to do exec with environ (but is not available on MacOs)
        // extern char** environ; // environment variables
        // execvpe(command->name, command->args, environ); // exec+args+path+environ

        /// This shows how to do exec with auto-path resolve
        // add a NULL argument to the end of args, and the name to the beginning
        // as required by exec

        // increase args size by 2
        command->args = (char **) realloc(
                command->args, sizeof(char *) * (command->arg_count += 2));

        // shift everything forward by 1
        for (int i = command->arg_count - 2; i > 0; --i)
            command->args[i] = command->args[i - 1];

        // set args[0] as a copy of name
        command->args[0] = strdup(command->name);
        // set args[arg_count-1] (last) to NULL
        command->args[command->arg_count - 1] = NULL;

        char path[20] = "/bin/";
        strcat(path, command->name);
        execv(path, command->args); // exec+args+path
        exit(0);
        /// TODO: do your own exec  path resolving using execv()
    } else {

        wait(0); 
       // wait for child process to finish
        return SUCCESS;
    }

    printf("-%s: %s: command not found\n", sysname, command->name);
    return UNKNOWN;

}





