#define _GNU_SOURCE
#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h> //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>

const char *sysname = "shellfyre";
char *paths[10];
char *searchPaths[10];
char *oldPaths[10];
const char *path;
bool isCd = false;

enum return_codes
{
	SUCCESS = 0,
	EXIT = 1,
	UNKNOWN = 2,
};

struct command_t
{
	char *name;
	bool background;
	bool auto_complete;
	int arg_count;
	char **args;
	char *redirects[3];		// in/out redirection
	struct command_t *next; // for piping
};

/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t *command)
{
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
	if (command->next)
	{
		printf("\tPiped to:\n");
		print_command(command->next);
	}
}

/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command)
{
	if (command->arg_count)
	{
		for (int i = 0; i < command->arg_count; ++i)
			free(command->args[i]);
		free(command->args);
	}
	for (int i = 0; i < 3; ++i)
		if (command->redirects[i])
			free(command->redirects[i]);
	if (command->next)
	{
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
int show_prompt()
{
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
int parse_command(char *buf, struct command_t *command)
{
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
	command->name = (char *)malloc(strlen(pch) + 1);
	if (pch == NULL)
		command->name[0] = 0;
	else
		strcpy(command->name, pch);

	command->args = (char **)malloc(sizeof(char *));

	int redirect_index;
	int arg_index = 0;
	char temp_buf[1024], *arg;

	while (1)
	{
		// tokenize input on splitters
		pch = strtok(NULL, splitters);
		if (!pch)
			break;
		arg = temp_buf;
		strcpy(arg, pch);
		len = strlen(arg);

		if (len == 0)
			continue;										 // empty arg, go for next
		while (len > 0 && strchr(splitters, arg[0]) != NULL) // trim left whitespace
		{
			arg++;
			len--;
		}
		while (len > 0 && strchr(splitters, arg[len - 1]) != NULL)
			arg[--len] = 0; // trim right whitespace
		if (len == 0)
			continue; // empty arg, go for next

		// piping to another command
		if (strcmp(arg, "|") == 0)
		{
			struct command_t *c = malloc(sizeof(struct command_t));
			int l = strlen(pch);
			pch[l] = splitters[0]; // restore strtok termination
			index = 1;
			while (pch[index] == ' ' || pch[index] == '\t')
				index++; // skip whitespaces

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
		if (arg[0] == '>')
		{
			if (len > 1 && arg[1] == '>')
			{
				redirect_index = 2;
				arg++;
				len--;
			}
			else
				redirect_index = 1;
		}
		if (redirect_index != -1)
		{
			command->redirects[redirect_index] = malloc(len);
			strcpy(command->redirects[redirect_index], arg + 1);
			continue;
		}

		// normal arguments
		if (len > 2 && ((arg[0] == '"' && arg[len - 1] == '"') || (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
		{
			arg[--len] = 0;
			arg++;
		}
		command->args = (char **)realloc(command->args, sizeof(char *) * (arg_index + 1));
		command->args[arg_index] = (char *)malloc(len + 1);
		strcpy(command->args[arg_index++], arg);
	}
	command->arg_count = arg_index;
	return 0;
}

void prompt_backspace()
{
	putchar(8);	  // go back 1
	putchar(' '); // write empty over
	putchar(8);	  // go back 1 again
}

/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command)
{
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

	// FIXME: backspace is applied before printing chars
	show_prompt();
	int multicode_state = 0;
	buf[0] = 0;

	while (1)
	{
		c = getchar();
		// printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

		if (c == 9) // handle tab
		{
			buf[index++] = '?'; // autocomplete
			break;
		}

		if (c == 127) // handle backspace
		{
			if (index > 0)
			{
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
		if (c == 91 && multicode_state == 1)
		{
			multicode_state = 2;
			continue;
		}
		if (c == 65 && multicode_state == 2) // up arrow
		{
			int i;
			while (index > 0)
			{
				prompt_backspace();
				index--;
			}
			for (i = 0; oldbuf[i]; ++i)
			{
				putchar(oldbuf[i]);
				buf[i] = oldbuf[i];
			}
			index = i;
			continue;
		}
		else
			multicode_state = 0;

		putchar(c); // echo the character
		buf[index++] = c;
		if (index >= sizeof(buf) - 1)
			break;
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

	//print_command(command); // DEBUG: uncomment for debugging

	// restore the old settings
	tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
	return SUCCESS;
}

int process_command(struct command_t *command);
void searchCommandPath(char *command_name, char *pth);
void fileSearch(char *file);
void recursiveFileSearch(char *file);
void cdh();
void take(char* fileName);

int main()
{
	int i = 0;
	
	while (1)
	{		
		FILE *fp;
  		char patho[1035];
  		fp = popen("/bin/pwd ", "r");
  		if (fp == NULL) 
  		{
    			printf("Failed to run command\n" );
  		}
	
		fgets(patho, sizeof(patho), fp);
  		pclose(fp);			
		char *cpy = strdup(patho);
		char *tkn = strtok(cpy, ":");
		if(isCd)
		{	
			while (tkn != NULL)
			{ 	
				isCd = false;
				oldPaths[i] = malloc(strlen(tkn) + 1);
				strcpy(oldPaths[i], tkn);
				tkn = strtok(NULL, ":");	
				//printf("%d--%s\n",i,oldPaths[i]);	
				i++;		
			}
			free(cpy);
		}
		
		struct command_t *command = malloc(sizeof(struct command_t));
		memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

		int code;
		code = prompt(command);
		if (code == EXIT)
			break;

		code = process_command(command);
		if (code == EXIT)
			break;

		free_command(command);
		
	}
	
	printf("\n");
	return 0;
}

int process_command(struct command_t *command)
{
	int r;
	if (strcmp(command->name, "") == 0)
		return SUCCESS;

	if (strcmp(command->name, "exit") == 0)
		return EXIT;

	if (strcmp(command->name, "cd") == 0)
	{
		if (command->arg_count > 0)
		{			
			r = chdir(command->args[0]);
			if (r == -1)
				printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
			else isCd = true;
			return SUCCESS;
		}
	}

	// TODO: Implement your custom commands here
	
	if (!strcmp(command->name, "cdh"))
	{
		cdh();
		return SUCCESS;
	}
	if (!strcmp(command->name, "take"))
	{
		char *fileName;		
		take(fileName);
		return SUCCESS;
	}
	pid_t pid = fork();

	if (pid == 0) // child
	{
		// increase args size by 2
		command->args = (char **)realloc(
			command->args, sizeof(char *) * (command->arg_count += 2));

		// shift everything forward by 1
		for (int i = command->arg_count - 2; i > 0; --i)
			command->args[i] = command->args[i - 1];

		// set args[0] as a cpy of name
		command->args[0] = strdup(command->name);
		// set args[arg_count-1] (last) to NULL
		command->args[command->arg_count - 1] = NULL;

		/// TODO: do your own exec with path resolving using execv()

		char *command_path = malloc(1024);
		searchCommandPath(command->name, command_path);
		
		if (strcmp(command_path, ""))
		{			
			execv(command_path, command->args); 
		}
		else if (!strcmp(command->name, "filesearch"))
		{	
			if (!strcmp(command->args[1], "-r"))
			{
				recursiveFileSearch(command->args[2]);
			}
			else
			{
				fileSearch(command->args[1]);
			}
		}
		
		else
		{
			printf("-%s: %s: command not found\n", sysname, command->name);
		}
	}
	else
	{
		/// TODO: Wait for child to finish if command is not running in background
		
		if (!command->background)
			wait(0);
		

		return SUCCESS;
	}
	
	
	exit(0);
	return UNKNOWN;
}

void searchCommandPath(char *command_name, char *pth)
{
	path = getenv("PATH");
	char *cpy = strdup(path);
	char *tkn = strtok(cpy, ":");
	int j = 0;
	while (tkn != NULL)
	{ 
		paths[j] = malloc(strlen(tkn) + 1);
		strcpy(paths[j], tkn);
		tkn = strtok(NULL, ":");
		
		j++;
	}
	free(cpy);

	int i;
	for (i = 0; i < sizeof(paths); i++)
	{
		if (paths[i] != NULL)
		{	
			char *file_name;
			DIR *d;
			struct dirent *dir;
			d = opendir(paths[i]);
			if (d != NULL)
			{ 
				while ((dir = readdir(d)) != NULL)
				{			
					file_name = dir->d_name;
					if (!strcmp(file_name, command_name))
					{
						//printf("FOUND: file_name: \"%s\"\n", file_name);
						char *p = malloc(1);
						strcat(p, paths[i]);
						strcat(p, "/");
						strcat(p, file_name);
						strcpy(pth, p);
						closedir(d);
						return;
					}
					
				}
				closedir(d);
			}
		}
	}
	strcpy(pth, "");
	return;
}

void fileSearch(char *file) 
{
	path = getenv("PWD");
	char *cpy = strdup(path);
	char *tkn = strtok(cpy, ":");
	int j = 0;
	while (tkn != NULL)
	{ 
		searchPaths[j] = malloc(strlen(tkn) + 1);
		strcpy(searchPaths[j], tkn);
		tkn = strtok(NULL, ":");
		
		j++;
	}
	free(cpy);

	int i;
	for (i = 0; i < sizeof(searchPaths); i++)
	{ 
		if (searchPaths[i] != NULL)
		{	
			char *file_name;
			DIR *d;
			struct dirent *dir;
			d = opendir(searchPaths[i]);
			if (d != NULL)
			{ 
				while ((dir = readdir(d)) != NULL)
				{			
					file_name = dir->d_name;
					if (strcasestr(file_name, file))
					{       
						printf("	./%s\n", file_name);
					}
					
				}
				closedir(d);
			}
		}
	}
	return;
}

void recursiveFileSearch(char *file) 
{
	path = getenv("PWD");
	char *cpy = strdup(path);
	char *tkn = strtok(cpy, ":");
	int j = 0;
	while (tkn != NULL)
	{ 
		searchPaths[j] = malloc(strlen(tkn) + 1);
		strcpy(searchPaths[j], tkn);
		tkn = strtok(NULL, ":");
		
		j++;
	}
	free(cpy);

	int i;
	char *file_name;
	for (i = 0; i < sizeof(searchPaths); i++)
	{ 
		if (searchPaths[i] != NULL)
		{			
			DIR *d;
			struct dirent *dir;
			d = opendir(searchPaths[i]);
			bool isDir = false;
			if(i>0) isDir = true;
			
			if (d != NULL)
			{ 	
				while ((dir = readdir(d)) != NULL)
				{		
					file_name = dir->d_name;
					
					struct stat path_stat;
    					stat(file_name, &path_stat);
    					if(!S_ISREG(path_stat.st_mode) && !(!strcmp(dir->d_name, ".") || !strcmp(dir->d_name, "..")))
    					{
    					char *cpy = strdup(file_name);
					char *tkn = strtok(cpy, ":");
					searchPaths[j] = malloc(strlen(tkn) + 1);
					strcpy(searchPaths[i + 1], tkn);  					
    					}
					if (strcasestr(file_name, file) && !isDir)
					{       
						printf("	./%s\n", file_name);
					}
					if (strcasestr(file_name, file) && isDir)
					{
						printf("	./%s/%s\n", searchPaths[i], file_name);
					}
				}
				closedir(d);
			}
		}
	}
	return;
}

void cdh()
{	
	char *chars = {"abcdefghik"};
	int i;
	int j; 
	int r;
	for (i = 0 ; i < sizeof(oldPaths) ; i++)
	{	
		if(oldPaths[0] == NULL)
		{
			printf("	!!!There is no previous directories!!!\n");
			return;
		}
		if(oldPaths[i] == NULL) break;		
		printf("	%c %d) %s",chars[i], i+1, oldPaths[i]);
	}
	char letter[1];
	printf("\nSelect directory by letter or number: ");
	gets(letter);
	printf("\n");
	
	for (i = 0 ; i < sizeof(oldPaths) ; i++)
	{	
		if (oldPaths[i] == NULL) break;

		if ((int)letter[0] - 48 == i + 1 || letter[0] == chars[i])
		{	
			char *path;
			path = (char*) malloc((strlen(oldPaths[i] + 1))*sizeof(char));
			for (j = 0 ; j < strlen(oldPaths[i]) && oldPaths[i][j] != '\n'; j++) 
			{
				//printf("--%c\n", oldPaths[i][j]); 
				path[j] = oldPaths[i][j]; 
			}
			path[j] = '\0';
			//printf("PATH: --%s--\n",path);

			//r = chdir("/home/musab/Desktop/Project1/Dir2");
			//break;
			r = chdir(path);
			if (r == -1)
				printf("-%s: %s\n", sysname, strerror(errno));
			free(path);
		}
	}
}

void take(char *fileName)
{
	fileName = calloc(100, sizeof(char));
	printf("\nEnter the path: "); 
	gets(fileName);
	
	char *tkn = strtok(fileName, "/");
	char *splitted[3];
	char *homeDir; 	
	int i = 0;
	int j;
	int r;
	while(tkn != NULL) 
	{
      		splitted[i] = malloc(strlen(tkn) + 1);
      		strcpy(splitted[i], tkn);
      		tkn = strtok(NULL, "/");
      		i++;
   	}
	
	for(j = 0; j < i; j++)
	{			
		homeDir = (char *) malloc(strlen(splitted[j]) + 21);
		strcat(homeDir,"/home/");   
		strcat(homeDir,getenv("USERNAME"));
		strcat(homeDir,"/");
		strcat(homeDir,splitted[j]);   
		strcat(homeDir,"\0");   
		
    		r = chdir(homeDir);
		//if (r == -1) printf("-%s: %s\n", sysname, strerror(errno));
		if (r == 0) continue;		 					
			
		char *args[] = {"/bin/mkdir",splitted[j],NULL};
	
		pid_t pid = fork();	
	
		if(pid == 0)
		{
			execv("/bin/mkdir",args);
				
		}
		else
		{
			wait(NULL);	
			chdir(splitted[j]); 
		}
	}
	free(fileName);
}


























