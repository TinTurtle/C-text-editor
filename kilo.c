/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <fcntl.h>
#include <stdarg.h>
#include <time.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
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

#define KILO_VERSION "0.0.1"

#define KILO_TAB_STOP 8

#define KILO_QUIT_TIMES 3

//special keys remapped to a different value for assigning purpose in the editor
enum editorKey{
	BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};


/*** data ***/

//for printing the text from the file
typedef struct erow{
	int size;
	int rsize;
	char *render;
	char *chars;
}erow;

struct editorConfig{
	int cx, cy;
	int rx;
	int rowoff; //for vertical scrolling
	int coloff;
	int screenrows;//no of rows available on the screen-depends on the screen size
	int screencols;// no of cols available on the screen
	struct termios orig_termios;//to store and edit the terminal attributes
	int numrows;//no of rows in the text of the opened file
    int dirty;
	char *filename;
	char statusmsg[80];
	time_t statusmsg_time;
	erow *row;// struct for storing and displaying the text in the file
};
struct editorConfig E;


/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);

/*** terminal ***/

void die(const char *s){
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void disableRawMode(){
	//tcsetattr is used to set the orig attrbutes to the terminal again at the end of the program
	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios)== -1)die("tcsetattr");
}

void enableRawMode(){
	//tcgetattr used to load the orig attributes of the terminal
	if(tcgetattr(STDIN_FILENO, &E.orig_termios)== -1)die("tcgetattr");
	//atexit is used to call the disableRawMode func at-exit
	atexit(disableRawMode);

	struct termios raw = E.orig_termios;
	// disabling thee flags that might hinder our keypresses
	raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
	raw.c_iflag &= ~(IXON | ICRNL | ISTRIP | BRKINT | INPCK);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag &= ~(CS8);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw)== -1) die("tcsetattr");
}

int editorReadKey(){
	int nread;
	char c;
	while((nread = read(STDIN_FILENO, &c, 1)) != 1){
		if(nread == -1 && errno != EAGAIN) die("read");
	} 

	if(c == '\x1b'){
		char seq[3];

		if(read(STDIN_FILENO, &seq[0], 1)!=1)return '\x1b';
		if(read(STDIN_FILENO, &seq[1], 1)!=1)return '\x1b';

		if(seq[0] == '['){
			if(seq[1] >= '0' && seq[1] <= '9'){
				if(read(STDIN_FILENO, &seq[2], 1)!=1) return '\x1b';
				if(seq[2]=='~'){
					switch(seq[1]){
						case '1': return HOME_KEY;
						case '3': return DEL_KEY;
						case '4': return END_KEY;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME_KEY;
						case '8': return END_KEY;
					}
				}
			}else{
				switch(seq[1]){
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}
		}else if(seq[0] == 'O'){
			switch(seq[1]){
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}

		return '\x1b';
	}
	else{
		return c;
	}

}



int getCursorPosition(int *rows, int *cols){
	char buf[32];
	unsigned int i = 0;


	if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4)return -1;

	while(i < sizeof(buf) - 1){
		if(read(STDOUT_FILENO, &buf[i], 1)!=1)return -1;
		if(buf[i]=='R')break;
		i++;
	}

	buf[i] = '\0';

	if(buf[0] != '\x1b' || buf[1] != '[')return -1;
	if(sscanf(&buf[2], "%d;%d", rows, cols) != 2)return -1;

	return 0;
}

int getWindowSize(int *rows, int *cols){
	struct winsize ws;

	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)== -1 || ws.ws_col == 0){
		if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B",12)!=12) return -1;
		return getCursorPosition(rows, cols);
		
	}else{
		*rows = ws.ws_row;
		*cols = ws.ws_col;
	
		return 0;
	}
}


/*** row operations ***/

int editorRowCxToRx(erow *row, int cx){
	int rx = 0;
	int j = 0;
	for(j=0;j<cx;j++){
		if(row->chars[j]=='\t'){
			rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
		}
		rx++;
	}
	return rx;
}

void editorUpdateRow(erow *row){
	int tabs = 0;
	int j;
	for(j=0;j<row->size;j++){
		if(row->chars[j] == '\t')tabs++;
	}

	free(row->render);
	row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);


	int idx = 0;
	for(j=0;j<row->size;j++){
		if(row->chars[j]=='\t'){
			row->render[idx++] = ' ';
			while(idx % KILO_TAB_STOP!=0) row->render[idx++]=' ';
		}else{
			row->render[idx++] = row->chars[j];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void editorAppendRow(char *s, size_t len){
	E.row = realloc(E.row, sizeof(erow)*(E.numrows + 1));

	int at = E.numrows;
	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';

	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editorUpdateRow(&E.row[at]);

	E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row){
    free(row->render);
    free(row->chars);
}

void editorDelRow(int at){
    if(at<0 && at>=E.numrows)return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at+1], sizeof(erow) * (E.numrows - at + 1));
    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c){
	if(at < 0 || at > row->size) at = row->size;
	row->chars = realloc(row->chars, row->size+2);
	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = c;
	editorUpdateRow(row);
    E.dirty++;
}

//for delete key
void editorRowDelChar(erow *row, int at){
    if(at<0 || at>= row->size)return;
    memmove(&row->chars[at],  &row->chars[at+1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c){
	if(E.cy == E.numrows) {
		editorAppendRow("", 0);
	}
	editorRowInsertChar(&E.row[E.cy], E.cx, c);
	E.cx++;
}

void editorDelChar(){
    if(E.cy == E.numrows)return;
    erow *row  = &E.row[E.cy];

    if(E.cx>0){
        editorRowDelChar(row, E.cx-1);
        E.cx--;
    }
}

/*** file i/o ***/

void editorOpen(char *filename){
	free(E.filename);
	//using strdup as it allocates a new storage block in the memory and then copies the original value to it
	E.filename = strdup(filename);//storing the filename for displaying it on the status bar
	FILE *fp = fopen(filename, "r");
	if(!fp) die("fopen");
	
	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	while((linelen = getline(&line, &linecap, fp))!= -1){
		while(linelen > 0 && (line[linelen-1]=='\n' || line[linelen-1]=='\r')) linelen --;

		editorAppendRow(line, linelen);
	}
	free(line);
	fclose(fp);
    
    E.dirty = 0;//because editorAppendRow is immediately called
}

char *editorRowsToString(int *buflen){
	int totlen = 0; 
	int j;
	for(j=0;j<E.numrows;j++)
		totlen += E.row[j].size - 1;
	*buflen = totlen;

	char *buf = malloc(totlen);
	char *p = buf;

	for(j = 0; j<E.numrows; j++){
		memcpy(p, E.row[j].chars, E.row[j].size);
		p += E.row[j].size;
		*p = '\n';
		p++;
	}
	return buf;
}


void editorSave(){
	if(E.filename == NULL) return;

	int len;
	char *buf = editorRowsToString(&len);


	int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
	if(fd != -1){ 
		if(ftruncate(fd, len) != -1){
			if(write(fd, buf, len) == len){
				close(fd);
				free(buf);
                E.dirty = 0;
				editorSetStatusMessage("%d bytes written to disk", len);
				return;
			}
		}
		close(fd);
	}
	free(buf);
	editorSetStatusMessage("Cannot Save File! I/O Error : %s", strerror(errno));
}

/*** append buffer ***/

//Dynamic String for storing the buffer
//this is done to avoid writing too many write statements to the terminal
struct abuf{
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}


void abAppend(struct abuf *ab, const char *s, int len){
	char *new = realloc(ab->b, ab->len+len);

	if(new == NULL)return;

	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void abFree(struct abuf *ab){
	free(ab->b);
}


/*** output ***/

void editorScroll(){
	E.rx = 0;
	if(E.cy < E.numrows){
		E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
	}

	if(E.cy<E.rowoff){
		E.rowoff = E.cy;
	}
	if(E.cy>=E.rowoff + E.screenrows){
		E.rowoff = E.cy - E.screenrows + 1;
	}
	if(E.rx < E.coloff){
		E.coloff = E.rx;
	}
	if(E.rx >= E.coloff + E.screencols){
		E.coloff = E.rx - E.screencols + 1;
	}
}

void editorDrawRows(struct abuf *ab){
	int y;
	for(y=0;y<E.screenrows;y++){
		int filerow = y + E.rowoff;
		if(filerow >= E.numrows){
			if(E.numrows == 0 && y == E.screenrows / 3){
				char welcome[80];
				int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
				if(welcomelen > E.screencols) welcomelen = E.screencols;
				int padding = (E.screencols - welcomelen)/2;
				if(padding){
					abAppend(ab, "~", 1);
					padding--;
				}
				while(padding--) abAppend(ab, " ", 1);
				abAppend(ab, welcome, welcomelen);
			}else{
				abAppend(ab, "~", 1);
			}
		}else{
			int len = E.row[filerow].rsize - E.coloff;
			if(len < 0) len = 0;
			if(len > E.screencols) len = E.screencols;
			abAppend(ab, &E.row[filerow].render[E.coloff], len);
		}
		abAppend(ab, "\x1b[K", 3);
		abAppend(ab, "\r\n", 2);

	}
}

void editorDrawStatusBar(struct abuf *ab){
	abAppend(ab, "\x1b[7m", 4);
	char status[80], rstatus[80];

	int len = snprintf(status, sizeof(status),"%.20s - %d lines %s", E.filename?E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : "");
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
	if(len > E.screencols) len = E.screencols;
	abAppend(ab, status, len);

	while( len < E.screencols){
		if(E.screencols - len == rlen){
			abAppend(ab, rstatus, rlen);
			break;
		}else{

			abAppend(ab, " ", 1);
			len++;
		}
	}
	abAppend(ab,"\x1b[m", 3);
	abAppend(ab,"\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab){
	abAppend(ab, "\x1b[K", 3);
	int msglen = strlen(E.statusmsg);
	if(msglen > E.screencols) msglen = E.screencols;
	if(msglen && time(NULL) - E.statusmsg_time < 5) abAppend(ab, E.statusmsg, msglen);
} 


void editorRefreshScreen(){
	editorScroll();

	struct abuf ab = ABUF_INIT;
	
	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H", 3);
	
	editorDrawRows(&ab);
	editorDrawStatusBar(&ab);
        editorDrawMessageBar(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);

	abAppend(&ab, buf, strlen(buf));
	abAppend(&ab, "\x1b[?25h", 6);
	

	write(STDOUT_FILENO, ab.b, ab.len);

	abFree(&ab);

}

void editorSetStatusMessage(const char *fmt, ...){
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
}

/*** input ***/

void editorMoveCursor(int key){
	erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

	switch(key){
		case ARROW_UP:
			if(E.cy != 0) E.cy--;
			break;
		case ARROW_LEFT:
			if(E.cx != 0) E.cx--;
			else if(E.cy > 0){
				E.cy--;
				E.cx = E.row[E.cy].size;
			}
			break;
		case ARROW_DOWN:
			if(E.cy < E.numrows) E.cy++;
			break;
		case ARROW_RIGHT:
			if(row && E.cx< row->size)E.cx++;
			else if(row && E.cx == row->size){
				E.cy++;
				E.cx = 0;
			}
			break;
	}
	//checks if the cursor is with in the line on each line
	
	row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	int rowlen = row ? row->size:0;
	if(E.cx>rowlen){
		E.cx = rowlen;
	}

}


void editorProcessKeypress(){
    static int quit_times =  KILO_QUIT_TIMES;

	int c = editorReadKey();

	switch (c) {
		case '\r':
			//to-do
			break;
		case CTRL_KEY('q'):
            if(E.dirty && quit_times > 0){
                editorSetStatusMessage("WARNING!!! File has unsaved changes. " "Press Ctrl-Q %d more times to quit.",quit_times);
                quit_times--;
                return;
            }
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);

			exit(0);
			break;
		case CTRL_KEY('s'):
			editorSave();
			break;
		case PAGE_UP:
		case PAGE_DOWN:
			{
				if(c == PAGE_UP){
					E.cy = E.rowoff;
				}else if (c == PAGE_DOWN){
					E.cy = E.rowoff + E.screenrows - 1;
					if(E.cy > E.numrows) E.cy = E.numrows;
				}

				int times = E.screenrows;
				
				while(times--){
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
				}
			}
			break;
		case HOME_KEY:
			if(E.cy<E.numrows) E.cx = E.row[E.cy].size;
			break;
		case END_KEY:
			E.cx = E.screencols - 1;
			break;
		case BACKSPACE:
		case CTRL_KEY('h'):
		case DEL_KEY:
            if(c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
			break;
		case ARROW_UP:
		case ARROW_LEFT:
		case ARROW_DOWN:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;
		case CTRL_KEY('l'):
		case '\x1b':
			break;
		default:
			editorInsertChar(c);
			break;
	}
}

/*** init ***/


void initEditor(){
	E.cx = 0;
	E.cy = 0;
	E.rx = 0;
	E.numrows = 0;
    E.dirty = 0;
	E.row = NULL;
	E.rowoff = 0;
	E.coloff = 0;
	E.filename = NULL;
  	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;

	if(getWindowSize(&E.screenrows, &E.screencols)==-1)die("getWindowSize");
	E.screenrows -= 2;
}

int main(int argc, char *argv[]){
	enableRawMode();
	initEditor();
	if(argc>=2){
		editorOpen(argv[1]);
	}
	
	editorSetStatusMessage("HELP: Ctrl + S = save | Ctrl + Q = quit");

	while(1){

		editorRefreshScreen();

		editorProcessKeypress();
	}	
	return 0;
}
