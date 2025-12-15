/*** includes ***/


#include <errno.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <termios.h>
//termios.h is the lib used for making this all possible
//we are rn using it to disable features like Ctrl+C and Ctrl+Z of the terminal 
//this helps us in using the Ctrl+alpha keypresses for recording bytes from 1-26
//this struct is used to store the original attributes of the terminal for this session hence the name orig_termios

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)


/*** data ***/

struct termios orig_termios;

/*** terminal ***/

void die(const char *s){
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void disableRawMode(){
	//tcsetattr is used to set the orig attrbutes to the terminal again at the end of the program
	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios)== -1)die("tcsetattr");
}

void enableRawMode(){
	//tcgetattr used to load the orig attributes of the terminal
	if(tcgetattr(STDIN_FILENO, &orig_termios)== -1)die("tcgetattr");
	//atexit is used to call the disableRawMode func at-exit
	atexit(disableRawMode);

	struct termios raw = orig_termios;
	// disabling thee flags that might hinder our keypresses
	raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
	raw.c_iflag &= ~(IXON | ICRNL | ISTRIP | BRKINT | INPCK);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag &= ~(CS8);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw)== -1) die("tcsetattr");
}

char editorReadKey(){
	int nread;
	char c;
	while((nread = read(STDIN_FILENO, &c, 1)) != 1){
		if(nread == -1 && errno != EAGAIN) die("read");
	} return c;
}

/*** output ***/

void editorDrawRows(){
	int y;
	for(y=0;y<24;y++){
		write(STDOUT_FILENO, "~\r\n", 3);
	}
}

void editorRefreshScreen(){
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);
	
	editorDrawRows();

	write(STDOUT_FILENO, "x1b[H",3);

}

/*** input ***/

void editorProcessKeypress(){
	char c = editorReadKey();

	switch (c) {
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);

			exit(0);
			break;
	}
}

/*** init ***/

int main(){
	editorRefreshScreen();
	enableRawMode();


	while(1){
		editorProcessKeypress();
	}	
	return 0;
}
